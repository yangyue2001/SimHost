#ifndef SIM_SERVICE_DIFFTEST_H
#define SIM_SERVICE_DIFFTEST_H

#include <cstdint>
#include <string>

#include "service/difftest/ref/ref.h"
#include "service/probe/probe.h"

// One DiffArchCommit probe is the DUT state for this cycle. on_posedge()
// steps the reference, then reads RvissRefState once. init() copies the
// functional RAM pointer into the reference when --diff is set. A skipped
// slot is not executed; any skip copies the DUT architectural state back.
class DifftestService : public ProbeService {
 public:
  DifftestService();
  ~DifftestService() override;

  const char* name() const override;
  void register_options(OptionSet& options) override;
  bool init(const SimContext& context) override;
  int on_posedge(SimContext& context) override;
  void finish(int trap) override;

 private:
  static bool apply_diff(void* context, const char* value);
  static bool apply_timeout(void* context, const char* value);

  RefLibrary ref_;
  std::string ref_path_;
  bool enabled_;
  // Cycles without a valid commit before this service aborts. 0 disables it.
  uint64_t timeout_cycles_;
  uint64_t idle_cycles_;
};

// Generated probes.cpp calls this. nwords includes coreid at words[0].
extern "C" void difftest_probe_push(const char* name, const uint64_t* words,
                                    int nwords);

#endif
