#ifndef SIM_SERVICE_SERVICE_H
#define SIM_SERVICE_SERVICE_H

#include <cstdint>

constexpr int STATE_GOOD = 0;
constexpr int STATE_ABORT = 1;
constexpr int STATE_LIMIT = 2;
constexpr int STATE_SIGNAL = 3;
constexpr int STATE_RUNNING = -1;

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
  // A service sets this after fork() resumes the checkpoint process.
  bool clone_requested;
  // The resumed checkpoint wants a waveform from the next cycle. -b/-e do
  // not apply to that recording.
  bool waveform_requested;
};

class OptionSet;

class SimService {
 public:
  virtual ~SimService() = default;
  virtual const char *name() const = 0;
  virtual void register_options(OptionSet &) {}
  // Return false to stop before the main loop.
  virtual bool init(const SimContext &) { return true; }
  // After the rising eval, before the falling edge. Return STATE_RUNNING or a
  // trap. context is mutable so a service can request at_clone().
  virtual int on_posedge(SimContext &) { return STATE_RUNNING; }
  virtual void at_clone() {}
  // Parent must not open a waveform. The resumed checkpoint asks for one.
  virtual bool defer_waveform() const { return false; }
  // trap is the reason the main loop stopped.
  virtual void finish(int) {}
};

#endif
