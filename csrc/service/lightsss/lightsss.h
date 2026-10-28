#ifndef SIM_SERVICE_LIGHTSSS_H
#define SIM_SERVICE_LIGHTSSS_H

#include <cstdint>
#include <deque>

#include "service/service.h"

struct LightsssShare;

// Fork checkpoints of the whole simulator. The parent keeps the newest two
// children asleep and does not record a waveform. On STATE_ABORT it wakes the
// oldest one. That process records from the next cycle, through the failing
// cycle, and 100 cycles past it. Good, limit, and signal exits kill the
// children instead. DRAMSim3 is not rebuilt: fork already copied its state.
class LightsssService : public SimService {
 public:
  LightsssService();
  ~LightsssService() override;

  const char* name() const override;
  void register_options(OptionSet& options) override;
  bool init(const SimContext& context) override;
  int on_posedge(SimContext& context) override;
  bool defer_waveform() const override;
  void finish(int trap) override;

 private:
  static bool apply_interval(void* context, const char* value);
  static bool apply_fail_at(void* context, const char* value);

  bool spawn(SimContext& context);
  int child_cycle(const SimContext& context);
  void wakeup(uint64_t cycle);
  void clear_children();

  bool enabled_;
  bool fail_enabled_;
  bool is_child_;
  bool have_forked_;
  uint64_t interval_ms_;
  uint64_t fail_cycle_;
  uint64_t last_cycle_;
  uint64_t last_fork_ms_;
  LightsssShare* share_;
  // front() is the newest checkpoint. back() is the oldest.
  std::deque<int> slots_;
};

#endif
