#include "service/difftest/difftest.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "service/memory/memory.h"
#include "session/options.h"

// Cycles without a commit before difftest fails. 0 disables the check.
constexpr uint64_t kDefaultCommitTimeout = 15000;

namespace {

DifftestService* current = nullptr;

// Bundle.scala DiffArchCommit, source order. words[0] is coreid.
// 2 + 6*width + 32 + 32 + 19 + 1. width is the commit Vec length.
constexpr int kSlotWords = 6;
constexpr int kGprCount = 32;
constexpr int kFprCount = 32;

constexpr int kSlotValid = 0;
constexpr int kSlotSkip = 1;
constexpr int kSlotInterrupt = 2;
constexpr int kSlotCause = 4;
constexpr int kSlotTval = 5;

enum {
  kPriv = 0,
  kMstatus,
  kSstatus,
  kMepc,
  kSepc,
  kMtval,
  kStval,
  kMtvec,
  kStvec,
  kMcause,
  kScause,
  kSatp,
  kMip,
  kMie,
  kMscratch,
  kSscratch,
  kMideleg,
  kMedeleg,
  kFcsr,
  kCsrCount
};

constexpr int kFixedWords = 2 + kGprCount + kFprCount + kCsrCount + 1;

// sstatus is not stored in the reference. It is these bits of mstatus.
constexpr uint64_t kSstatusReadMask =
    (1ull << 1) | (1ull << 5) | (1ull << 8) | (3ull << 13) | (1ull << 18) |
    (1ull << 19) | (3ull << 32) | (1ull << 63);

const char* kCsrName[kCsrCount] = {
    "privilege", "mstatus", "sstatus",  "mepc",     "sepc",    "mtval",
    "stval",     "mtvec",   "stvec",    "mcause",   "scause",  "satp",
    "mip",       "mie",     "mscratch", "sscratch", "mideleg", "medeleg",
    "fcsr",
};

struct ArchProbe {
  int width;
  const uint64_t* slots;
  const uint64_t* xrf;
  const uint64_t* frf;
  const uint64_t* csr;
  uint64_t pc;
};

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

uint64_t csr_ref(const RvissRefState& ref, int index) {
  switch (index) {
    case kPriv:
      return ref.priv;
    case kMstatus:
      return ref.mstatus;
    case kSstatus:
      return ref.mstatus & kSstatusReadMask;
    case kMepc:
      return ref.mepc;
    case kSepc:
      return ref.sepc;
    case kMtval:
      return ref.mtval;
    case kStval:
      return ref.stval;
    case kMtvec:
      return ref.mtvec;
    case kStvec:
      return ref.stvec;
    case kMcause:
      return ref.mcause;
    case kScause:
      return ref.scause;
    case kSatp:
      return ref.satp;
    case kMip:
      return ref.mip;
    case kMie:
      return ref.mie;
    case kMscratch:
      return ref.mscratch;
    case kSscratch:
      return ref.sscratch;
    case kMideleg:
      return ref.mideleg;
    case kMedeleg:
      return ref.medeleg;
    case kFcsr:
      return ref.fcsr;
    default:
      return 0;
  }
}

bool parse_arch(const ProbeSample& sample, ArchProbe* out) {
  if (sample.nwords <= kFixedWords ||
      (sample.nwords - kFixedWords) % kSlotWords != 0) {
    std::printf("difftest probe %s has %d words, expected DiffArchCommit\n",
                sample.name, sample.nwords);
    return false;
  }
  if (sample.words[0] != 0) {
    std::printf("difftest probe %s hart %" PRIu64 " is not supported\n",
                sample.name, sample.words[0]);
    return false;
  }
  if (sample.words[1] == 0) {
    std::printf("difftest probe %s valid is 0\n", sample.name);
    return false;
  }
  const int width = (sample.nwords - kFixedWords) / kSlotWords;
  int cursor = 2;
  out->width = width;
  out->slots = sample.words + cursor;
  cursor += width * kSlotWords;
  out->xrf = sample.words + cursor;
  cursor += kGprCount;
  out->frf = sample.words + cursor;
  cursor += kFprCount;
  out->csr = sample.words + cursor;
  cursor += kCsrCount;
  out->pc = sample.words[cursor];
  return true;
}

// Keep CSRs the probe does not carry. sstatus is a view of mstatus.
void overlay_dut(RvissRefState* state, const ArchProbe& arch) {
  for (int index = 0; index < kGprCount; ++index) {
    state->gpr[index] = arch.xrf[index];
    state->fpr[index] = arch.frf[index];
  }
  state->gpr[0] = 0;
  state->pc = arch.pc;
  state->priv = arch.csr[kPriv];
  state->mstatus = arch.csr[kMstatus];
  state->mepc = arch.csr[kMepc];
  state->sepc = arch.csr[kSepc];
  state->mtval = arch.csr[kMtval];
  state->stval = arch.csr[kStval];
  state->mtvec = arch.csr[kMtvec];
  state->stvec = arch.csr[kStvec];
  state->mcause = arch.csr[kMcause];
  state->scause = arch.csr[kScause];
  state->satp = arch.csr[kSatp];
  state->mip = arch.csr[kMip];
  state->mie = arch.csr[kMie];
  state->mscratch = arch.csr[kMscratch];
  state->sscratch = arch.csr[kSscratch];
  state->mideleg = arch.csr[kMideleg];
  state->medeleg = arch.csr[kMedeleg];
  state->fcsr = arch.csr[kFcsr];
}

bool legal_priv(uint64_t priv) { return priv == 0 || priv == 1 || priv == 3; }

void print_regs(const char* kind, const char* who, const uint64_t* value,
                int count) {
  std::printf("difftest %s %s\n", who, kind);
  for (int index = 0; index < count; ++index) {
    std::printf("  %s%-2d %016" PRIx64 "\n", kind, index, value[index]);
  }
}

void print_mismatch(uint64_t cycle, const char* name, uint64_t dut,
                    uint64_t ref_value) {
  std::printf("difftest mismatch cycle %" PRIu64 " %s dut %016" PRIx64
              " ref %016" PRIx64 "\n",
              cycle, name, dut, ref_value);
}

}  // namespace

DifftestService::DifftestService()
    : enabled_(false), timeout_cycles_(kDefaultCommitTimeout), idle_cycles_(0) {
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
  service->enabled_ = true;
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
  if (!enabled_) {
    return true;
  }

  if (!ref_.open(ref_path_.c_str())) {
    return false;
  }
  uint8_t* ram = memory_ram_data();
  const uint64_t bytes = memory_ram_bytes();
  if (ram == nullptr || bytes == 0) {
    std::fprintf(stderr, "difftest cannot see functional RAM\n");
    ref_.close();
    return false;
  }
  ref_.memcpy(kFunctionalRamBase, ram, static_cast<size_t>(bytes),
              DIFFTEST_TO_REF);
  return true;
}

void DifftestService::finish(int) {
  clear();
  ref_.close();
}

int DifftestService::on_posedge(SimContext& context) {
  if (failed()) {
    std::printf("difftest %s\n", fail_reason());
    clear();
    return STATE_ABORT;
  }

  if (!enabled_) {
    clear();
    return STATE_RUNNING;
  }

  const ProbeSample* sample = nullptr;
  for (const ProbeSample& pending_sample : pending()) {
    if (std::strcmp(pending_sample.name, "DiffArchCommit") != 0) {
      std::printf("unknown difftest probe %s\n", pending_sample.name);
      clear();
      return STATE_ABORT;
    }
    if (sample != nullptr) {
      std::printf("difftest more than one DiffArchCommit\n");
      clear();
      return STATE_ABORT;
    }
    sample = &pending_sample;
  }

  bool saw_commit = false;
  bool skipped = false;
  ArchProbe arch{};
  if (sample != nullptr) {
    if (!parse_arch(*sample, &arch)) {
      clear();
      return STATE_ABORT;
    }
    for (int index = 0; index < arch.width; ++index) {
      const uint64_t* slot = arch.slots + index * kSlotWords;
      if (slot[kSlotValid] == 0) {
        for (int later = index + 1; later < arch.width; ++later) {
          if (arch.slots[later * kSlotWords + kSlotValid] != 0) {
            std::printf("difftest mismatch cycle %" PRIu64
                        " commit %d follows an empty slot\n",
                        context.cycle, later);
            clear();
            return STATE_ABORT;
          }
        }
        break;
      }
      saw_commit = true;
      if (slot[kSlotInterrupt] != 0) {
        const uint64_t cause = slot[kSlotCause];
        if ((cause & ~(1ull << 63)) >= 64) {
          std::printf("difftest mismatch cycle %" PRIu64
                      " trap cause %016" PRIx64 " is out of range\n",
                      context.cycle, cause);
          clear();
          return STATE_ABORT;
        }
        ref_.raise_trap(cause, slot[kSlotTval]);
        continue;
      }
      if (slot[kSlotSkip] != 0) {
        // Younger slots have no separate result. The DUT image covers them.
        skipped = true;
        break;
      }
      ref_.exec(1);
    }
  }

  if (saw_commit) {
    idle_cycles_ = 0;
  } else if (timeout_cycles_ != 0) {
    idle_cycles_ += 1;
  }

  if (sample != nullptr) {
    RvissRefState ref;
    ref_.regcpy(&ref, DIFFTEST_TO_DUT);
    if (skipped) {
      if (!legal_priv(arch.csr[kPriv])) {
        std::printf("difftest mismatch cycle %" PRIu64
                    " privilege %" PRIu64 " is not U, S, or M\n",
                    context.cycle, arch.csr[kPriv]);
        clear();
        return STATE_ABORT;
      }
      overlay_dut(&ref, arch);
      ref_.regcpy(&ref, DIFFTEST_TO_REF);
    } else if (ref.pc != arch.pc) {
      print_mismatch(context.cycle, "pc", arch.pc, ref.pc);
      clear();
      return STATE_ABORT;
    } else {
      for (int index = 0; index < kGprCount; ++index) {
        if (arch.xrf[index] == ref.gpr[index]) {
          continue;
        }
        char name[8];
        std::snprintf(name, sizeof(name), "x%d", index);
        print_mismatch(context.cycle, name, arch.xrf[index], ref.gpr[index]);
        print_regs("x", "dut", arch.xrf, kGprCount);
        print_regs("x", "ref", ref.gpr, kGprCount);
        clear();
        return STATE_ABORT;
      }
      for (int index = 0; index < kFprCount; ++index) {
        if (arch.frf[index] == ref.fpr[index]) {
          continue;
        }
        char name[8];
        std::snprintf(name, sizeof(name), "f%d", index);
        print_mismatch(context.cycle, name, arch.frf[index], ref.fpr[index]);
        print_regs("f", "dut", arch.frf, kFprCount);
        print_regs("f", "ref", ref.fpr, kFprCount);
        clear();
        return STATE_ABORT;
      }
      for (int index = 0; index < kCsrCount; ++index) {
        const uint64_t dut = arch.csr[index];
        const uint64_t ref_value = csr_ref(ref, index);
        if (dut == ref_value) {
          continue;
        }
        print_mismatch(context.cycle, kCsrName[index], dut, ref_value);
        std::printf("difftest csr\n");
        for (int field = 0; field < kCsrCount; ++field) {
          std::printf("  %-10s dut %016" PRIx64 " ref %016" PRIx64 "\n",
                      kCsrName[field], arch.csr[field], csr_ref(ref, field));
        }
        clear();
        return STATE_ABORT;
      }
    }
  }

  if (timeout_cycles_ != 0 && idle_cycles_ >= timeout_cycles_) {
    std::printf("difftest timeout cycle %" PRIu64 " no commit for %" PRIu64
                " cycles\n",
                context.cycle, idle_cycles_);
    clear();
    return STATE_ABORT;
  }
  clear();
  return STATE_RUNNING;
}
