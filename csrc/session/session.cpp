#include "session/session.h"

#include <cstdio>
#include <utility>
#include <vector>

#include "platform/platform.h"
#include "service/service.h"
#include "session/args.h"
#include "session/registry.h"
#include "session/signals.h"

namespace {

static inline void drive_half(Platform* platform, unsigned clock_level) {
  platform->set_clock(clock_level);
  platform->eval();
}

SimArgsView make_view(const SessionArgs& args) {
  SimArgsView view;
  view.max_cycles = args.max_cycles;
  view.reset_cycles = args.reset_cycles;
  view.seed = args.seed;
  view.log_begin = args.log_begin;
  view.log_end = args.log_end;
  view.dump_wave = args.dump_wave;
  view.dump_wave_full = args.dump_wave_full;
  view.wave_path = args.wave_path.c_str();
  return view;
}

}  // namespace

Session::Session() : argc_(0), argv_(nullptr), ready_(false) {}

const char* trap_label(int trap) {
  switch (trap) {
    case STATE_GOOD:
      return "GOOD TRAP";
    case STATE_LIMIT:
      return "EXCEEDING CYCLE LIMIT";
    case STATE_SIGNAL:
      return "SIGNAL";
    default:
      return "ABORT";
  }
}

int trap_exit_code(int trap) {
  if (trap == STATE_GOOD || trap == STATE_LIMIT) {
    return 0;
  }
  return 1;
}

SessionStartup Session::open(int argc, char** argv) {
  ready_ = false;
  services_ = make_services();
  OptionSet options;
  for (const std::unique_ptr<SimService>& service : services_) {
    service->register_options(options);
  }

  SessionArgs args;
  ParseStatus parsed = parse_args(argc, argv, options, &args);
  if (parsed == ParseStatus::Help) {
    return SessionStartup::kHelp;
  }
  if (parsed == ParseStatus::Error) {
    return SessionStartup::kError;
  }
  args_ = std::move(args);
  argc_ = argc;
  argv_ = argv;
  ready_ = true;
  return SessionStartup::kReady;
}

SessionResult Session::run() {
  SessionResult result;
  if (!ready_) {
    // started stays false, so main exits without printing a trap line.
    return result;
  }

  SessionArgs& args = args_;
  std::unique_ptr<Platform> platform = create_platform(argc_, argv_, args.seed);
  platform->set_clock(0);
  platform->set_reset(1);
  platform->eval();

  if (args.dump_wave) {
    if (!platform->trace_compiled()) {
      std::fprintf(stderr,
                   "waveform was requested, but this binary was built without "
                   "EMU_TRACE\n");
      return result;
    }
    if (args.wave_path.empty()) {
      args.wave_path = platform->default_waveform_name();
    }
    if (!platform->waveform_open(args.wave_path.c_str())) {
      std::fprintf(stderr, "cannot open waveform file %s\n",
                   args.wave_path.c_str());
      return result;
    }
  }

  // Reset is not an architectural cycle, so it does not move the timestamp.
  for (uint32_t index = 0; index < args.reset_cycles; ++index) {
    platform->set_reset(1);
    drive_half(platform.get(), 1);
    drive_half(platform.get(), 0);
  }

  // Reset is already low, but the first post-reset edge is in the main loop.
  platform->set_reset(0);

  SimArgsView view = make_view(args);
  SimContext context;
  context.cycle = 0;
  context.args = &view;
  for (const std::unique_ptr<SimService>& service : services_) {
    if (!service->init(context)) {
      return result;
    }
  }
  // Assertions stay off through reset. --no-assert keeps them off afterwards.
  platform->set_assertions_enabled(args.enable_assert);

  // -b, -e, and --max-cycles count architectural cycles. Waveform time is
  // cycle * 2, and a full trace also records the falling edge at cycle * 2 + 1.
  int trap = STATE_RUNNING;
  uint64_t cycle = 0;
  while (trap == STATE_RUNNING && cycle < args.max_cycles) {
    if (signal_requested()) {
      trap = STATE_SIGNAL;
      break;
    }

    context.cycle = cycle;
    drive_half(platform.get(), 1);
#if defined(EMU_TRACE_VCD) || defined(EMU_TRACE_FST)
    if (args.dump_wave && args.log_begin <= cycle && cycle <= args.log_end) {
      platform->waveform_dump(cycle * 2);
    }
#endif

    int service_trap = STATE_RUNNING;
    for (const std::unique_ptr<SimService>& service : services_) {
      int returned = service->on_posedge(context);
      if (returned != STATE_RUNNING) {
        service_trap = returned;
        break;
      }
    }
    drive_half(platform.get(), 0);
#if defined(EMU_TRACE_VCD) || defined(EMU_TRACE_FST)
    if (args.dump_wave_full && args.log_begin <= cycle &&
        cycle <= args.log_end) {
      platform->waveform_dump(cycle * 2 + 1);
    }
#endif

    uint64_t exit_value = platform->exit_code();
    if (exit_value == ~uint64_t{0}) {
      trap = STATE_GOOD;
    } else if (exit_value != 0) {
      trap = STATE_ABORT;
    } else {
      trap = service_trap;
    }
    cycle += 1;
  }
  if (trap == STATE_RUNNING) {
    trap = STATE_LIMIT;
  }

  for (const std::unique_ptr<SimService>& service : services_) {
    service->finish();
  }
  platform->close_waveform();

  result.started = true;
  result.trap = trap;
  result.cycle = cycle;
  return result;
}
