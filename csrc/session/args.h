#ifndef SIM_SESSION_ARGS_H
#define SIM_SESSION_ARGS_H

#include <cstdint>
#include <limits>
#include <string>

#include "session/options.h"

struct SessionArgs {
  uint64_t max_cycles = std::numeric_limits<uint64_t>::max();
  uint32_t reset_cycles = 50;
  uint32_t seed = 0;
  uint64_t log_begin = 0;
  uint64_t log_end = std::numeric_limits<uint64_t>::max();
  bool dump_wave = false;
  bool dump_wave_full = false;
  std::string wave_path;
};

enum class ParseStatus {
  Ok,
  Help,
  Error,
};

ParseStatus parse_args(int argc, char **argv, OptionSet &extra,
                       SessionArgs *args);

#endif
