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

bool in_trace_window(uint64_t cycle, const SessionArgs& args) {
  return args.log_begin <= cycle && cycle <= args.log_end;
}

void drive_half(Platform* platform, unsigned clock_level,
                const SessionArgs& args, bool resetting) {
  platform->set_clock(clock_level);
  platform->eval();
  if (resetting && args.dump_wave_full && args.log_begin == 0) {
    platform->waveform_dump();
  }
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

Session::Session() : ready_(false) {}

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
  ready_ = true;
  return SessionStartup::kReady;
}

SessionResult Session::run() {
  SessionResult result;
  if (!ready_) {
    return result;
  }

  SessionArgs& args = args_;
  std::unique_ptr<Platform> platform = create_platform(args.seed);
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
    uint64_t start_time =
        args.dump_wave_full ? args.log_begin * 2 : args.log_begin;
    if (!platform->waveform_open(start_time, args.wave_path.c_str())) {
      std::fprintf(stderr, "cannot open waveform file %s\n",
                   args.wave_path.c_str());
      return result;
    }
  }

  for (uint32_t index = 0; index < args.reset_cycles; ++index) {
    platform->set_reset(1);
    drive_half(platform.get(), 1, args, true);
    drive_half(platform.get(), 0, args, true);
  }
  platform->set_reset(0);

  SimArgsView view = make_view(args);
  SimContext context;
  context.cycle = 0;
  context.args = &view;
  for (const std::unique_ptr<SimService>& service : services_) {
    service->init(context);
  }
  platform->set_assertions_enabled(true);

  int trap = STATE_RUNNING;
  uint64_t cycle = 0;
  while (trap == STATE_RUNNING) {
    if (signal_requested()) {
      trap = STATE_SIGNAL;
      break;
    }
    if (cycle >= args.max_cycles) {
      trap = STATE_LIMIT;
      break;
    }

    platform->set_clock(1);
    platform->eval();
    if (args.dump_wave && in_trace_window(cycle, args)) {
      platform->waveform_dump();
    }
    context.cycle = cycle;
    for (const std::unique_ptr<SimService>& service : services_) {
      service->on_phase(SimPhase::AfterPosedge, context);
    }
    for (const std::unique_ptr<SimService>& service : services_) {
      service->on_phase(SimPhase::MidCycle, context);
    }

    platform->set_clock(0);
    platform->eval();
    if (args.dump_wave_full && in_trace_window(cycle, args)) {
      platform->waveform_dump();
    }
    for (const std::unique_ptr<SimService>& service : services_) {
      service->on_phase(SimPhase::AfterNegedge, context);
    }
    for (const std::unique_ptr<SimService>& service : services_) {
      service->on_phase(SimPhase::EndCycle, context);
    }

    uint64_t exit_value = platform->exit_code();
    if (exit_value == ~uint64_t{0}) {
      trap = STATE_GOOD;
    } else if (exit_value != 0) {
      trap = STATE_ABORT;
    } else {
      for (const std::unique_ptr<SimService>& service : services_) {
        int service_trap = service->poll_trap();
        if (service_trap != STATE_RUNNING) {
          trap = service_trap;
          break;
        }
      }
    }
    cycle += 1;
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
