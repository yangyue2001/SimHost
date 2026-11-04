#ifndef SIM_SERVICE_DIFFTEST_H
#define SIM_SERVICE_DIFFTEST_H

#include <cstdint>
#include <string>

#include "service/difftest/diffstate.h"
#include "service/difftest/ref/ref.h"
#include "service/probe/probe.h"

// Compare one cycle of DiffState with the reference.
//
// Verilog fills DiffState during the rising edge. Typed DPI writes it
// directly. Generated probes are unpacked here. on_posedge() then compares.
// It does not split that into a commit check and an xrf check.
//
// init() is the only place that looks at --diff. After it returns, either
// the reference is open or this service stays idle. on_posedge() does not
// check the option again. With nothing to compare it returns. A skip commit
// is not compared. Its pc and integer write are copied back with regcpy,
// the same way NEMU skips one instruction.
// Any other mismatch is remembered and printed in finish().
//
// While the reference is open, cycles with no valid commit are counted.
// --diff-timeout (default 15000, 0 disables) fails through that same path.
// A snapshot does not count as a commit. A skip commit does.
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
  enum MismatchKind {
    kNone = 0,
    kValue = 1,
    kOverflow = 2,
    kRange = 3,
    kTimeout = 4
  };

  // Architectural file tracked from commits, then replaced by a snapshot.
  struct Arch {
    uint64_t gpr[32];
    uint64_t fpr[32];
    DiffCsr csr;
    uint64_t pc;
  };

  struct Mismatch {
    bool set;
    int kind;
    uint64_t cycle;
    char name[32];
    uint64_t dut_value;
    uint64_t ref_value;
    bool have_ref;
    bool dump_fpr;
    bool dump_csr;
    Arch dut_arch;
    Arch ref_arch;
  };

  static bool apply_diff(void* context, const char* value);
  static bool apply_timeout(void* context, const char* value);
  int stop(int trap);
  int fail(int kind, uint64_t cycle, const char* name, uint64_t dut_value,
           uint64_t ref_value, const RvissRefState* ref, bool dump_fpr,
           bool dump_csr);
  int commit_timeout(uint64_t cycle);
  void read_ref(RvissRefState* state);
  void apply_integer(const DiffCommitSlot& commit);
  void sync_skip(const DiffCommitSlot& commit, RvissRefState* state);
  void take_snapshots(const DiffState& state);
  int compare_gpr(uint64_t cycle, const RvissRefState& ref, bool dump_fpr,
                  bool dump_csr);
  int compare_fpr(uint64_t cycle, const RvissRefState& ref, bool dump_csr);
  int compare_csr(uint64_t cycle, const RvissRefState& ref, bool dump_fpr);
  void print_mismatch() const;

  RefLibrary ref_;
  std::string ref_path_;
  Arch dut_;
  bool enabled_;
  bool csr_known_;
  bool fpr_known_;
  Mismatch mismatch_;
  uint64_t checked_;
  // Cycles without a valid commit before this service aborts. 0 disables it.
  uint64_t timeout_cycles_;
  uint64_t idle_cycles_;
};

// Generated probes.cpp calls this. nwords includes coreid at words[0].
extern "C" void difftest_probe_push(const char* name, const uint64_t* words,
                                    int nwords);

#endif
