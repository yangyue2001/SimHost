#include "service/difftest/difftest.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "session/options.h"

namespace {

DifftestService* current = nullptr;

// Cycles without a commit before difftest fails. 0 disables the check.
constexpr uint64_t kDefaultCommitTimeout = 15000;

bool parse_u64(const char* text, const char* option, uint64_t* out) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  *out = static_cast<uint64_t>(value);
  return true;
}

// Scala field order. words[0] is coreid. See
// scala/src/main/scala/difftest/Bundle.scala.
constexpr int kCommitWords = 9;
constexpr int kXrfWords = 33;
constexpr int kFprWords = 33;
constexpr int kCsrWords = 19;

struct CsrField {
  const char* name;
  uint64_t DiffCsr::* dut;
};

// DUT probe order. privilege and sstatus are derived from the reference.
const CsrField kCsrFields[] = {
    {"privilege", &DiffCsr::privilege},
    {"mstatus", &DiffCsr::mstatus},
    {"sstatus", &DiffCsr::sstatus},
    {"mepc", &DiffCsr::mepc},
    {"sepc", &DiffCsr::sepc},
    {"mtval", &DiffCsr::mtval},
    {"stval", &DiffCsr::stval},
    {"mtvec", &DiffCsr::mtvec},
    {"stvec", &DiffCsr::stvec},
    {"mcause", &DiffCsr::mcause},
    {"scause", &DiffCsr::scause},
    {"satp", &DiffCsr::satp},
    {"mip", &DiffCsr::mip},
    {"mie", &DiffCsr::mie},
    {"mscratch", &DiffCsr::mscratch},
    {"sscratch", &DiffCsr::sscratch},
    {"mideleg", &DiffCsr::mideleg},
    {"medeleg", &DiffCsr::medeleg},
};

// sstatus is not stored. ISS reads it as these bits of mstatus.
constexpr uint64_t kSstatusReadMask =
    (1ull << 1) | (1ull << 5) | (1ull << 8) | (3ull << 13) | (1ull << 18) |
    (1ull << 19) | (3ull << 32) | (1ull << 63);

uint64_t ref_csr(const RvissRefState& src, uint64_t DiffCsr::* field) {
  if (field == &DiffCsr::privilege) return src.priv;
  if (field == &DiffCsr::sstatus) return src.mstatus & kSstatusReadMask;
  if (field == &DiffCsr::mstatus) return src.mstatus;
  if (field == &DiffCsr::mepc) return src.mepc;
  if (field == &DiffCsr::sepc) return src.sepc;
  if (field == &DiffCsr::mtval) return src.mtval;
  if (field == &DiffCsr::stval) return src.stval;
  if (field == &DiffCsr::mtvec) return src.mtvec;
  if (field == &DiffCsr::stvec) return src.stvec;
  if (field == &DiffCsr::mcause) return src.mcause;
  if (field == &DiffCsr::scause) return src.scause;
  if (field == &DiffCsr::satp) return src.satp;
  if (field == &DiffCsr::mip) return src.mip;
  if (field == &DiffCsr::mie) return src.mie;
  if (field == &DiffCsr::mscratch) return src.mscratch;
  if (field == &DiffCsr::sscratch) return src.sscratch;
  if (field == &DiffCsr::mideleg) return src.mideleg;
  if (field == &DiffCsr::medeleg) return src.medeleg;
  return 0;
}

void copy_csr(DiffCsr* dst, const RvissRefState& src) {
  for (const CsrField& field : kCsrFields) {
    dst->*(field.dut) = ref_csr(src, field.dut);
  }
}

bool has_commit(const DiffState& state) {
  for (int index = 0; index < kDiffCommitWidth; ++index) {
    if (state.commit[index].valid != 0) {
      return true;
    }
  }
  return false;
}

bool has_activity(const DiffState& state) {
  if (state.commit_overflow != 0 || state.csr_valid != 0 ||
      state.xrf_valid != 0 || state.fpr_valid != 0) {
    return true;
  }
  for (int index = 0; index < kDiffCommitWidth; ++index) {
    if (state.commit[index].valid != 0) {
      return true;
    }
  }
  return false;
}

void print_gpr(const char* who, const uint64_t value[32]) {
  std::printf("difftest %s gpr\n", who);
  for (int index = 0; index < 32; ++index) {
    std::printf("  x%-2d %016" PRIx64 "\n", index, value[index]);
  }
}

void print_fpr(const char* who, const uint64_t value[32]) {
  std::printf("difftest %s fpr\n", who);
  for (int index = 0; index < 32; ++index) {
    std::printf("  f%-2d %016" PRIx64 "\n", index, value[index]);
  }
}

void print_csr(const char* who, const DiffCsr& csr) {
  std::printf("difftest %s csr\n", who);
  for (const CsrField& field : kCsrFields) {
    std::printf("  %-10s %016" PRIx64 "\n", field.name, csr.*(field.dut));
  }
}

uint64_t instruction_bytes(uint32_t inst) { return (inst & 3u) != 3u ? 2 : 4; }

bool check_layout(const ProbeSample& sample, int expect) {
  if (sample.nwords != expect) {
    std::printf("difftest probe %s has %d words, expected %d\n", sample.name,
                sample.nwords, expect);
    return false;
  }
  if (sample.words[0] != 0) {
    std::printf("difftest probe %s hart %" PRIu64 " is not supported\n",
                sample.name, sample.words[0]);
    return false;
  }
  return true;
}

bool apply_commit(const ProbeSample& sample) {
  if (!check_layout(sample, kCommitWords)) {
    return false;
  }
  const uint64_t* word = sample.words;
  difftest_commit(
      static_cast<unsigned char>(word[2]), static_cast<unsigned char>(word[1]),
      static_cast<unsigned char>(word[3]), static_cast<unsigned char>(word[4]),
      word[5], static_cast<unsigned int>(word[6]), word[7],
      static_cast<unsigned char>(word[8]));
  return true;
}

bool apply_xrf(const ProbeSample& sample) {
  if (!check_layout(sample, kXrfWords)) {
    return false;
  }
  const uint64_t* word = sample.words;
  difftest_xrf(word[1], word[2], word[3], word[4], word[5], word[6], word[7],
               word[8], word[9], word[10], word[11], word[12], word[13],
               word[14], word[15], word[16], word[17], word[18], word[19],
               word[20], word[21], word[22], word[23], word[24], word[25],
               word[26], word[27], word[28], word[29], word[30], word[31],
               word[32]);
  return true;
}

bool apply_fpr(const ProbeSample& sample) {
  if (!check_layout(sample, kFprWords)) {
    return false;
  }
  const uint64_t* word = sample.words;
  difftest_fpr(word[1], word[2], word[3], word[4], word[5], word[6], word[7],
               word[8], word[9], word[10], word[11], word[12], word[13],
               word[14], word[15], word[16], word[17], word[18], word[19],
               word[20], word[21], word[22], word[23], word[24], word[25],
               word[26], word[27], word[28], word[29], word[30], word[31],
               word[32]);
  return true;
}

bool apply_csr(const ProbeSample& sample) {
  if (!check_layout(sample, kCsrWords)) {
    return false;
  }
  const uint64_t* word = sample.words;
  difftest_csr(word[1], word[2], word[3], word[4], word[5], word[6], word[7],
               word[8], word[9], word[10], word[11], word[12], word[13],
               word[14], word[15], word[16], word[17], word[18]);
  return true;
}

// Known names only. An unknown probe is a design error, not a second queue.
bool apply_sample(const ProbeSample& sample) {
  if (std::strcmp(sample.name, "Commit") == 0) {
    return apply_commit(sample);
  }
  if (std::strcmp(sample.name, "Xrf") == 0) {
    return apply_xrf(sample);
  }
  if (std::strcmp(sample.name, "Fpr") == 0) {
    return apply_fpr(sample);
  }
  if (std::strcmp(sample.name, "Csr") == 0) {
    return apply_csr(sample);
  }
  std::printf("unknown difftest probe %s\n", sample.name);
  return false;
}

}  // namespace

DifftestService::DifftestService()
    : enabled_(false),
      csr_known_(false),
      fpr_known_(false),
      checked_(0),
      timeout_cycles_(kDefaultCommitTimeout),
      idle_cycles_(0) {
  std::memset(&dut_, 0, sizeof(dut_));
  std::memset(&mismatch_, 0, sizeof(mismatch_));
  current = this;
}

DifftestService::~DifftestService() {
  finish(STATE_RUNNING);
  if (current == this) {
    current = nullptr;
  }
}

extern "C" void difftest_probe_push(const char* name, const uint64_t* words,
                                    int nwords) {
  if (current == nullptr) {
    std::fprintf(stderr, "difftest probe before the service exists\n");
    std::abort();
  }
  current->push(name, words, nwords);
}

const char* DifftestService::name() const { return "difftest"; }

bool DifftestService::apply_diff(void* context, const char* value) {
  auto* service = static_cast<DifftestService*>(context);
  if (value == nullptr || value[0] == '\0') {
    std::fprintf(stderr, "--diff needs a reference library path\n");
    return false;
  }
  service->ref_path_ = value;
  return true;
}

bool DifftestService::apply_timeout(void* context, const char* value) {
  auto* service = static_cast<DifftestService*>(context);
  uint64_t cycles = 0;
  if (!parse_u64(value, "diff-timeout", &cycles)) {
    return false;
  }
  service->timeout_cycles_ = cycles;
  return true;
}

void DifftestService::register_options(OptionSet& options) {
  options.add_value("diff", &DifftestService::apply_diff, this,
                    "path of the reference model shared library");
  options.add_value(
      "diff-timeout", &DifftestService::apply_timeout, this,
      "cycles without a commit before failure (default 15000, 0 disables)");
}

bool DifftestService::init(const SimContext&) {
  // Reset is not compared. Drop anything sampled before the main loop.
  discard();
  diffstate_clear();
  std::memset(&dut_, 0, sizeof(dut_));
  std::memset(&mismatch_, 0, sizeof(mismatch_));
  csr_known_ = false;
  fpr_known_ = false;
  checked_ = 0;
  idle_cycles_ = 0;
  enabled_ = false;
  // --diff is accepted or rejected here. on_posedge() does not look again.
  if (ref_path_.empty()) {
    return true;
  }
  if (!ref_.open(ref_path_.c_str())) {
    return false;
  }
  // Memory is initialized first. The reference runs that same program.
  if (!ref_.load_image()) {
    ref_.close();
    return false;
  }
  enabled_ = true;
  return true;
}

int DifftestService::stop(int trap) {
  diffstate_retire();
  return trap;
}

int DifftestService::fail(int kind, uint64_t cycle, const char* name,
                          uint64_t dut_value, uint64_t ref_value,
                          const RvissRefState* ref, bool dump_fpr,
                          bool dump_csr) {
  if (!mismatch_.set) {
    mismatch_.set = true;
    mismatch_.kind = kind;
    mismatch_.cycle = cycle;
    std::snprintf(mismatch_.name, sizeof(mismatch_.name), "%s",
                  name != nullptr ? name : "");
    mismatch_.dut_value = dut_value;
    mismatch_.ref_value = ref_value;
    mismatch_.have_ref = ref != nullptr;
    mismatch_.dump_fpr = dump_fpr;
    mismatch_.dump_csr = dump_csr;
    mismatch_.dut_arch = dut_;
    if (ref != nullptr) {
      std::memcpy(mismatch_.ref_arch.gpr, ref->gpr, sizeof(ref->gpr));
      std::memcpy(mismatch_.ref_arch.fpr, ref->fpr, sizeof(ref->fpr));
      mismatch_.ref_arch.pc = ref->pc;
      copy_csr(&mismatch_.ref_arch.csr, *ref);
    }
  }
  return stop(STATE_ABORT);
}

int DifftestService::commit_timeout(uint64_t cycle) {
  if (timeout_cycles_ == 0 || idle_cycles_ < timeout_cycles_) {
    return STATE_RUNNING;
  }
  RvissRefState ref;
  read_ref(&ref);
  return fail(kTimeout, cycle, "commit", idle_cycles_, timeout_cycles_, &ref,
              fpr_known_, csr_known_);
}

void DifftestService::read_ref(RvissRefState* state) {
  std::memset(state, 0, sizeof(*state));
  ref_.regcpy(state, DIFFTEST_TO_DUT);
  state->gpr[0] = 0;
}

void DifftestService::apply_integer(const DiffCommitSlot& commit) {
  if (commit.rfwen != 0 && commit.wdest != 0 && commit.wdest < 32) {
    dut_.gpr[commit.wdest] = commit.wdata;
  }
  dut_.gpr[0] = 0;
}

void DifftestService::sync_skip(const DiffCommitSlot& commit,
                                RvissRefState* state) {
  const uint64_t step = instruction_bytes(commit.inst);
  const int wen = commit.rfwen != 0 && commit.wdest != 0 ? 1 : 0;
  // NEMU skips by copying the architectural state back. The reference does
  // not execute the instruction.
  state->pc += step;
  if (wen != 0) {
    state->gpr[commit.wdest] = commit.wdata;
  }
  state->gpr[0] = 0;
  ref_.regcpy(state, DIFFTEST_TO_REF);
  apply_integer(commit);
  dut_.pc = commit.pc + step;
}

void DifftestService::take_snapshots(const DiffState& state) {
  if (state.xrf_valid != 0) {
    for (int index = 0; index < 32; ++index) {
      dut_.gpr[index] = state.xrf[index];
    }
  }
  if (state.fpr_valid != 0) {
    for (int index = 0; index < 32; ++index) {
      dut_.fpr[index] = state.fpr[index];
    }
    fpr_known_ = true;
  }
  if (state.csr_valid != 0) {
    dut_.csr = state.csr;
    csr_known_ = true;
  }
}

int DifftestService::compare_gpr(uint64_t cycle, const RvissRefState& ref,
                                 bool dump_fpr, bool dump_csr) {
  for (int index = 0; index < 32; ++index) {
    if (dut_.gpr[index] == ref.gpr[index]) {
      continue;
    }
    char name[8];
    std::snprintf(name, sizeof(name), "x%d", index);
    return fail(kValue, cycle, name, dut_.gpr[index], ref.gpr[index], &ref,
                dump_fpr, dump_csr);
  }
  return STATE_RUNNING;
}

int DifftestService::compare_fpr(uint64_t cycle, const RvissRefState& ref,
                                 bool dump_csr) {
  for (int index = 0; index < 32; ++index) {
    if (dut_.fpr[index] == ref.fpr[index]) {
      continue;
    }
    char name[8];
    std::snprintf(name, sizeof(name), "f%d", index);
    return fail(kValue, cycle, name, dut_.fpr[index], ref.fpr[index], &ref,
                true, dump_csr);
  }
  return STATE_RUNNING;
}

int DifftestService::compare_csr(uint64_t cycle, const RvissRefState& ref,
                                 bool dump_fpr) {
  for (const CsrField& field : kCsrFields) {
    const uint64_t dut_value = dut_.csr.*(field.dut);
    const uint64_t ref_value = ref_csr(ref, field.dut);
    if (dut_value == ref_value) {
      continue;
    }
    return fail(kValue, cycle, field.name, dut_value, ref_value, &ref, dump_fpr,
                true);
  }
  return STATE_RUNNING;
}

void DifftestService::print_mismatch() const {
  if (mismatch_.kind == kOverflow) {
    std::printf("difftest mismatch cycle %" PRIu64 " more than %d commits\n",
                mismatch_.cycle, kDiffCommitWidth);
  } else if (mismatch_.kind == kTimeout) {
    std::printf("difftest timeout cycle %" PRIu64 " no commit for %" PRIu64
                " cycles\n",
                mismatch_.cycle, mismatch_.dut_value);
  } else if (mismatch_.kind == kRange) {
    std::printf("difftest mismatch cycle %" PRIu64 " rd %" PRIu64
                " is out of range\n",
                mismatch_.cycle, mismatch_.dut_value);
  } else {
    std::printf("difftest mismatch cycle %" PRIu64 " %s dut %016" PRIx64
                " ref %016" PRIx64 "\n",
                mismatch_.cycle, mismatch_.name, mismatch_.dut_value,
                mismatch_.ref_value);
  }
  std::printf("difftest dut pc %016" PRIx64 "\n", mismatch_.dut_arch.pc);
  print_gpr("dut", mismatch_.dut_arch.gpr);
  if (mismatch_.dump_fpr) {
    print_fpr("dut", mismatch_.dut_arch.fpr);
  }
  if (mismatch_.dump_csr) {
    print_csr("dut", mismatch_.dut_arch.csr);
  }
  if (!mismatch_.have_ref) {
    return;
  }
  std::printf("difftest ref pc %016" PRIx64 "\n", mismatch_.ref_arch.pc);
  print_gpr("ref", mismatch_.ref_arch.gpr);
  if (mismatch_.dump_fpr) {
    print_fpr("ref", mismatch_.ref_arch.fpr);
  }
  if (mismatch_.dump_csr) {
    print_csr("ref", mismatch_.ref_arch.csr);
  }
}

int DifftestService::on_posedge(SimContext& context) {
  if (failed()) {
    std::printf("difftest %s\n", fail_reason());
    clear();
    return stop(STATE_ABORT);
  }
  for (const ProbeSample& sample : pending()) {
    if (!apply_sample(sample)) {
      clear();
      return stop(STATE_ABORT);
    }
  }
  clear();

  // init() already opened the reference, or left this service idle.
  if (!enabled_) {
    return stop(STATE_RUNNING);
  }
  const DiffState& state = diffstate();
  if (state.commit_overflow != 0) {
    RvissRefState ref;
    read_ref(&ref);
    return fail(kOverflow, context.cycle, "commit", 0, 0, &ref, fpr_known_,
                csr_known_);
  }

  // Count this cycle before the empty-cycle return. Only a valid commit,
  // including a skip, resets the timer. A snapshot does not.
  if (!has_commit(state)) {
    if (timeout_cycles_ != 0) {
      idle_cycles_ += 1;
    }
  } else {
    idle_cycles_ = 0;
  }

  if (!has_activity(state)) {
    const int timed_out = commit_timeout(context.cycle);
    if (timed_out != STATE_RUNNING) {
      return timed_out;
    }
    return stop(STATE_RUNNING);
  }

  const bool dump_fpr = fpr_known_;
  const bool dump_csr = csr_known_;
  for (int index = 0; index < kDiffCommitWidth; ++index) {
    const DiffCommitSlot& commit = state.commit[index];
    if (commit.valid == 0) {
      continue;
    }
    RvissRefState ref;
    read_ref(&ref);
    if (commit.rfwen != 0 && commit.wdest >= 32) {
      return fail(kRange, context.cycle, "rd", commit.wdest, 0, &ref, dump_fpr,
                  dump_csr);
    }
    // Skip is not compared. Sync the write and let the next commit check pc.
    if (commit.skip != 0) {
      sync_skip(commit, &ref);
      checked_ += 1;
      continue;
    }
    dut_.pc = commit.pc;
    if (ref.pc != commit.pc) {
      return fail(kValue, context.cycle, "pc", commit.pc, ref.pc, &ref,
                  dump_fpr, dump_csr);
    }
    ref_.exec(1);
    if (commit.rfwen != 0 && commit.wdest == 0 && commit.wdata != 0) {
      read_ref(&ref);
      return fail(kValue, context.cycle, "x0", commit.wdata, 0, &ref, dump_fpr,
                  dump_csr);
    }
    apply_integer(commit);
    read_ref(&ref);
    const int trap = compare_gpr(context.cycle, ref, dump_fpr, dump_csr);
    if (trap != STATE_RUNNING) {
      return trap;
    }
    // The next commit checks the reference pc. Do not assume pc + 4.
    dut_.pc = ref.pc;
    checked_ += 1;
  }

  take_snapshots(state);
  if (state.xrf_valid != 0) {
    RvissRefState ref;
    read_ref(&ref);
    const int trap = compare_gpr(context.cycle, ref, fpr_known_, csr_known_);
    if (trap != STATE_RUNNING) {
      return trap;
    }
  }
  if (state.fpr_valid != 0) {
    RvissRefState ref;
    read_ref(&ref);
    const int trap = compare_fpr(context.cycle, ref, csr_known_);
    if (trap != STATE_RUNNING) {
      return trap;
    }
  }
  if (state.csr_valid != 0) {
    RvissRefState ref;
    read_ref(&ref);
    const int trap = compare_csr(context.cycle, ref, fpr_known_);
    if (trap != STATE_RUNNING) {
      return trap;
    }
  }
  // Snapshots were compared above. No commit this cycle can still time out.
  const int timed_out = commit_timeout(context.cycle);
  if (timed_out != STATE_RUNNING) {
    return timed_out;
  }
  return stop(STATE_RUNNING);
}

void DifftestService::finish(int) {
  if (mismatch_.set) {
    print_mismatch();
    mismatch_.set = false;
  }
  if (ref_.opened()) {
    std::printf("difftest checked %" PRIu64 " commits\n", checked_);
  }
  clear();
  diffstate_clear();
  ref_.close();
}
