#ifndef SIM_SERVICE_SERVICE_H
#define SIM_SERVICE_SERVICE_H

#include <cstdint>

constexpr int STATE_GOOD = 0;
constexpr int STATE_ABORT = 1;
constexpr int STATE_LIMIT = 2;
constexpr int STATE_SIGNAL = 3;
constexpr int STATE_RUNNING = -1;

enum class SimPhase {
  AfterPosedge,
  MidCycle,
  AfterNegedge,
  EndCycle,
};

struct SimArgsView {
  uint64_t max_cycles;
  uint32_t reset_cycles;
  uint32_t seed;
  uint64_t log_begin;
  uint64_t log_end;
  bool dump_wave;
  bool dump_wave_full;
  const char *wave_path;
};

struct SimContext {
  uint64_t cycle;
  const SimArgsView *args;
};

class OptionSet;

class SimService {
 public:
  virtual ~SimService() = default;
  virtual const char *name() const = 0;
  virtual void register_options(OptionSet &) {}
  virtual void init(const SimContext &) {}
  virtual void on_phase(SimPhase, const SimContext &) {}
  virtual int poll_trap() const { return STATE_RUNNING; }
  virtual void at_clone() {}
  virtual void finish() {}
};

#endif
