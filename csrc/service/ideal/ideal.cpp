#include "service/ideal/ideal.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

extern "C" uint64_t memory_read(uint64_t offset);

namespace {

constexpr uint64_t kResetPc = 0x80000000ull;

IdealBackend* current_backend = nullptr;

void emit(const IdealHart& hart, unsigned char branch, unsigned char taken,
          uint64_t target, const IdealMmio& mmio) {
  uint64_t gpr[32];
  for (unsigned i = 0; i < 32; ++i) {
    gpr[i] = ideal_gpr_read(&hart, static_cast<uint8_t>(i));
  }
  ideal_backend_resp(
      gpr[0], gpr[1], gpr[2], gpr[3],
      gpr[4], gpr[5], gpr[6], gpr[7],
      gpr[8], gpr[9], gpr[10], gpr[11],
      gpr[12], gpr[13], gpr[14], gpr[15],
      gpr[16], gpr[17], gpr[18], gpr[19],
      gpr[20], gpr[21], gpr[22], gpr[23],
      gpr[24], gpr[25], gpr[26], gpr[27],
      gpr[28], gpr[29], gpr[30], gpr[31],
      hart.fpr[0], hart.fpr[1], hart.fpr[2], hart.fpr[3],
      hart.fpr[4], hart.fpr[5], hart.fpr[6], hart.fpr[7],
      hart.fpr[8], hart.fpr[9], hart.fpr[10], hart.fpr[11],
      hart.fpr[12], hart.fpr[13], hart.fpr[14], hart.fpr[15],
      hart.fpr[16], hart.fpr[17], hart.fpr[18], hart.fpr[19],
      hart.fpr[20], hart.fpr[21], hart.fpr[22], hart.fpr[23],
      hart.fpr[24], hart.fpr[25], hart.fpr[26], hart.fpr[27],
      hart.fpr[28], hart.fpr[29], hart.fpr[30], hart.fpr[31],
      hart.pc, hart.priv, hart.satp, hart.mstatus,
      hart.mepc, hart.mtvec, hart.mcause, hart.mtval,
      branch, taken, target,
      mmio.valid, mmio.store, mmio.size, mmio.addr, mmio.data);
}

}  // namespace

IdealBackend::IdealBackend() : executed_(0) { reset(); }

IdealBackend::~IdealBackend() { finish(STATE_RUNNING); }

const char* IdealBackend::name() const { return "ideal"; }

void IdealBackend::reset() {
  std::memset(&hart_, 0, sizeof(hart_));
  hart_.pc = kResetPc;
  hart_.priv = 3;
  hart_.mstatus = kMstatusUxl | kMstatusSxl;
  hart_.stimecmp = ~0ull;
  executed_ = 0;
  ideal_uart_reset();
}

bool IdealBackend::init(const SimContext&) {
  if (current_backend != nullptr) {
    std::fprintf(stderr, "only one ideal backend is supported\n");
    return false;
  }
  reset();
  ideal_decode_init();
  current_backend = this;
  return true;
}

void IdealBackend::finish(int) {
  if (current_backend == this) {
    if (executed_ != 0) {
      std::printf("ideal backend executed %" PRIu64 " instructions, pc %016" PRIx64
                  "\n",
                  executed_, hart_.pc);
    }
    current_backend = nullptr;
  }
}

void IdealBackend::publish(unsigned char branch, unsigned char taken,
                           uint64_t target, const IdealMmio& mmio) const {
  emit(hart_, branch, taken, target, mmio);
}

void IdealBackend::step(uint64_t pc, uint32_t inst, unsigned char page_fault,
                        unsigned char access_fault) {
  // A wrong-path fetch is not this instruction. Leave the hart alone.
  static uint64_t hist_pc[8];
  static uint32_t hist_inst[8];
  static int hist_n = 0;
  if (pc == hart_.pc) {
    hist_pc[hist_n & 7] = pc;
    hist_inst[hist_n & 7] = inst;
    hist_n++;
  }
  if (pc == 0x8000f000ull && pc == hart_.pc) {
    static int fetches = 0;
    if (fetches < 2) {
      std::fprintf(stderr, "FETCH pc=%lx inst=%08x pf=%u af=%u\n", pc, inst, page_fault, access_fault);
      int begin = hist_n > 8 ? hist_n - 8 : 0;
      for (int i = begin; i < hist_n; ++i) {
        std::fprintf(stderr, "  HIST pc=%lx inst=%08x\n", hist_pc[i & 7], hist_inst[i & 7]);
      }
    }
    fetches++;
  }
  if (pc == hart_.pc && pc >= 0x80000000ull && pc < 0x88000000ull) {
    static int mismatches = 0;
    uint64_t byte = pc - 0x80000000ull;
    uint64_t aligned = byte & ~7ull;
    unsigned shift = static_cast<unsigned>(byte - aligned) * 8u;
    uint64_t lo = memory_read(aligned);
    uint64_t hi = memory_read(aligned + 8);
    unsigned bits_in_lo = 64u - shift;
    uint32_t mem = static_cast<uint32_t>(lo >> shift);
    if (bits_in_lo < 32u) {
      mem |= static_cast<uint32_t>(hi) << bits_in_lo;
    }
    bool compressed = (mem & 3u) != 3u;
    uint32_t expect = compressed ? (mem & 0xffffu) : mem;
    uint32_t seen = compressed ? (inst & 0xffffu) : inst;
    if (seen != expect && mismatches < 24) {
      std::fprintf(stderr, "MISMATCH pc=%lx got=%08x mem=%08x\n", pc, inst, mem);
      mismatches++;
    }
  }
  if (pc != hart_.pc) {
    publish(0, 0, 0, IdealMmio{});
    return;
  }

  IdealContext ctx{};
  ctx.hart = &hart_;
  ctx.inst = static_cast<uint32_t>(inst);
  ctx.len = 4;
  ctx.npc = pc + 4;

  ideal_update_timer(&hart_);
  uint64_t interrupt = ideal_pending_interrupt(&hart_);
  // Fetch faults are taken before the bits are decoded. A pending interrupt
  // is taken instead of the instruction, including one that just became due.
  if (interrupt != 0) {
    ideal_raise(&ctx, interrupt, 0);
  } else if (page_fault != 0) {
    ideal_raise(&ctx, kExcInstPage, pc);
  } else if (access_fault != 0) {
    ideal_raise(&ctx, kExcInstAccess, pc);
  } else if ((pc & 1ull) != 0) {
    ideal_raise(&ctx, kExcInstMisaligned, pc);
  } else {
    uint32_t widened = 0;
    int len = ideal_widen(inst, &widened);
    if (len == 0) {
      ideal_raise(&ctx, kExcIllegal, inst);
    } else {
      ctx.len = static_cast<unsigned>(len);
      ctx.inst = widened;
      ctx.npc = pc + static_cast<unsigned>(len);
      IdealDecoded decoded;
      if (!ideal_decode(widened, &decoded) || decoded.exec == nullptr) {
        ideal_raise(&ctx, kExcIllegal, inst);
      } else {
        ctx.operands = decoded.operands;
        int status = decoded.exec(&ctx);
        if (status != STATE_RUNNING) {
          ideal_fix_sd(&hart_);
          publish(0, 0, 0, ctx.mmio);
          return;
        }
      }
    }
  }

  hart_.pc = ctx.npc;
  unsigned char branch = 0;
  unsigned char taken = 0;
  uint64_t target = 0;
  // Conditional branches only. Jumps and traps show up as the new pc.
  bool retired = !ctx.trapped;
  if (retired) {
    executed_ += 1;
    if ((ctx.inst & 0x7fu) == 0x63u) {
      branch = 1;
      taken = hart_.pc != pc + ctx.len ? 1 : 0;
      target = hart_.pc;
    }
  }
  ideal_count(&hart_, retired);
  ideal_fix_sd(&hart_);
  publish(branch, taken, target, ctx.mmio);
}

IdealHart* IdealBackend::hart() { return &hart_; }

IdealHart* ideal_hart() {
  return current_backend == nullptr ? nullptr : current_backend->hart();
}

extern "C" void ideal_backend_inst(uint64_t pc, unsigned int inst,
                                   unsigned char page_fault,
                                   unsigned char access_fault) {
  if (current_backend == nullptr) {
    emit(IdealHart{}, 0, 0, 0, IdealMmio{});
    return;
  }
  current_backend->step(pc, inst, page_fault, access_fault);
}
