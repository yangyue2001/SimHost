#include "session/args.h"

#include <getopt.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

enum {
  OPT_RESET_CYCLES = 1000,
  OPT_DUMP_WAVE,
  OPT_DUMP_WAVE_FULL,
  OPT_WAVE_PATH,
  OPT_ASSERT,
  OPT_NO_ASSERT,
  OPT_SERVICE_BASE = 2000,
};

void print_help(const char *argv0, const OptionSet &extra) {
  std::printf("Usage: %s [OPTION...]\n", argv0);
  std::printf("  -h, --help                 show this help and exit\n");
  std::printf(
      "  -C, --max-cycles=N         stop after N architectural cycles (returns "
      "0)\n");
  std::printf(
      "      --reset-cycles=N       reset for N cycles before the main loop "
      "(default 50)\n");
  std::printf(
      "  -s, --seed=N               Verilator reset seed; 0 keeps reset "
      "deterministic\n");
  std::printf(
      "  -b, --log-begin=N          first architectural cycle that may be "
      "traced\n");
  std::printf(
      "  -e, --log-end=N            last architectural cycle that may be "
      "traced\n");
  std::printf(
      "      --dump-wave            trace only the rising half-cycle\n");
  std::printf("      --dump-wave-full       trace both half-cycles\n");
  std::printf("      --wave-path=FILE       waveform output path\n");
  std::printf(
      "      --assert               enable Verilog assertions after reset "
      "(default)\n");
  std::printf(
      "      --no-assert            keep Verilog assertions disabled\n");
  for (const RegisteredOption &option : extra.items()) {
    if (option.help.empty()) {
      continue;
    }
    if (option.takes_value) {
      std::printf("      --%s=VAL\n", option.name.c_str());
    } else {
      std::printf("      --%s\n", option.name.c_str());
    }
    std::printf("          %s\n", option.help.c_str());
  }
}

bool parse_u64(const char *text, const char *option, uint64_t *out) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  errno = 0;
  char *end = nullptr;
  unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  *out = static_cast<uint64_t>(value);
  return true;
}

bool parse_u32_range(const char *text, const char *option, uint32_t *out,
                     uint32_t minimum, uint32_t maximum) {
  uint64_t value = 0;
  if (!parse_u64(text, option, &value)) {
    return false;
  }
  if (value < minimum || value > maximum) {
    std::fprintf(stderr, "--%s must be in range [%" PRIu32 ", %" PRIu32 "]\n",
                 option, minimum, maximum);
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

}  // namespace

ParseStatus parse_args(int argc, char **argv, OptionSet &extra,
                       SessionArgs *args) {
  std::vector<std::string> owned;
  std::vector<char *> pointers;
  owned.reserve(static_cast<size_t>(argc));
  pointers.reserve(static_cast<size_t>(argc) + 1);
  for (int index = 0; index < argc; ++index) {
    owned.emplace_back(argv[index]);
  }
  for (std::string &arg : owned) {
    pointers.push_back(&arg[0]);
  }
  pointers.push_back(nullptr);

  std::vector<option> longs;
  auto add_long = [&longs](const char *name, int has_arg, int value) {
    option spec;
    spec.name = name;
    spec.has_arg = has_arg;
    spec.flag = nullptr;
    spec.val = value;
    longs.push_back(spec);
  };
  add_long("help", no_argument, 'h');
  add_long("max-cycles", required_argument, 'C');
  add_long("reset-cycles", required_argument, OPT_RESET_CYCLES);
  add_long("seed", required_argument, 's');
  add_long("log-begin", required_argument, 'b');
  add_long("log-end", required_argument, 'e');
  add_long("dump-wave", no_argument, OPT_DUMP_WAVE);
  add_long("dump-wave-full", no_argument, OPT_DUMP_WAVE_FULL);
  add_long("wave-path", required_argument, OPT_WAVE_PATH);
  add_long("assert", no_argument, OPT_ASSERT);
  add_long("no-assert", no_argument, OPT_NO_ASSERT);
  const std::vector<RegisteredOption> &service_options = extra.items();
  for (size_t index = 0; index < service_options.size(); ++index) {
    add_long(
        service_options[index].name.c_str(),
        service_options[index].takes_value ? required_argument : no_argument,
        static_cast<int>(OPT_SERVICE_BASE + index));
  }
  option terminator;
  terminator.name = nullptr;
  terminator.has_arg = 0;
  terminator.flag = nullptr;
  terminator.val = 0;
  longs.push_back(terminator);

  optind = 1;
  opterr = 1;
  while (true) {
    int value =
        getopt_long(argc, pointers.data(), "hC:s:b:e:", longs.data(), nullptr);
    if (value == -1) {
      break;
    }
    switch (value) {
      case 'h':
        print_help(pointers[0], extra);
        return ParseStatus::Help;
      case 'C':
        if (!parse_u64(optarg, "max-cycles", &args->max_cycles)) {
          return ParseStatus::Error;
        }
        break;
      case 's':
        if (!parse_u32_range(
                optarg, "seed", &args->seed, 0,
                static_cast<uint32_t>(std::numeric_limits<int>::max()))) {
          return ParseStatus::Error;
        }
        break;
      case 'b':
        if (!parse_u64(optarg, "log-begin", &args->log_begin)) {
          return ParseStatus::Error;
        }
        break;
      case 'e':
        if (!parse_u64(optarg, "log-end", &args->log_end)) {
          return ParseStatus::Error;
        }
        break;
      case OPT_RESET_CYCLES:
        if (!parse_u32_range(optarg, "reset-cycles", &args->reset_cycles, 1,
                             std::numeric_limits<uint32_t>::max())) {
          return ParseStatus::Error;
        }
        break;
      case OPT_DUMP_WAVE:
        args->dump_wave = true;
        break;
      case OPT_DUMP_WAVE_FULL:
        args->dump_wave = true;
        args->dump_wave_full = true;
        break;
      case OPT_WAVE_PATH:
        args->wave_path = optarg;
        break;
      case OPT_ASSERT:
        args->enable_assert = true;
        break;
      case OPT_NO_ASSERT:
        args->enable_assert = false;
        break;
      case '?':
        return ParseStatus::Error;
      default:
        if (value >= OPT_SERVICE_BASE) {
          size_t index = static_cast<size_t>(value - OPT_SERVICE_BASE);
          if (index >= service_options.size() ||
              !service_options[index].apply(service_options[index].context,
                                            optarg)) {
            return ParseStatus::Error;
          }
          break;
        }
        return ParseStatus::Error;
    }
  }

  if (optind < argc) {
    std::fprintf(stderr, "unexpected argument: %s\n", pointers[optind]);
    return ParseStatus::Error;
  }
  if (args->log_begin > args->log_end) {
    std::fprintf(stderr,
                 "--log-begin must be less than or equal to --log-end\n");
    return ParseStatus::Error;
  }
  if (args->dump_wave &&
      args->log_begin > std::numeric_limits<uint64_t>::max() / 2) {
    std::fprintf(stderr, "--log-begin is too large for a waveform timestamp\n");
    return ParseStatus::Error;
  }
  return ParseStatus::Ok;
}
