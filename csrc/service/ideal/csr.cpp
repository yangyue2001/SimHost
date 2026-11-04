#include "service/ideal/exec.h"

#include "service/service.h"

namespace {

constexpr uint64_t kSstatusWrite =
    kMstatusSie | kMstatusSpie | kMstatusSpp | kMstatusFs | kMstatusSum |
    kMstatusMxr;
constexpr uint64_t kMstatusWrite =
    kSstatusWrite | kMstatusMie | kMstatusMpie | kMstatusMpp | kMstatusMprv |
    kMstatusTvm | kMstatusTw | kMstatusTsr;
constexpr uint64_t kMedelegMask =
    (1ull << kExcInstMisaligned) | (1ull << kExcInstAccess) |
    (1ull << kExcIllegal) | (1ull << kExcBreakpoint) |
    (1ull << kExcLoadMisaligned) | (1ull << kExcLoadAccess) |
    (1ull << kExcStoreMisaligned) | (1ull << kExcStoreAccess) |
    (1ull << kExcEcallU) | (1ull << kExcEcallS) | (1ull << kExcInstPage) |
    (1ull << kExcLoadPage) | (1ull << kExcStorePage);
constexpr uint64_t kSieMask = (1ull << 1) | (1ull << 5) | (1ull << 9);
constexpr uint64_t kMieMask = kSieMask | (1ull << 3) | (1ull << 7) | (1ull << 11);
constexpr uint64_t kCounterMask = 0x7ull;
constexpr uint64_t kMenvcfgStce = 1ull << 63;
constexpr uint64_t kMenvcfgMask =
    (1ull << 0) | (3ull << 4) | (1ull << 6) | (1ull << 7) | (1ull << 62) |
    kMenvcfgStce;
constexpr uint64_t kStip = 1ull << 5;
constexpr uint64_t kPmpAddrMask = (1ull << 54) - 1;
constexpr unsigned kPmpEntries = 16;

uint64_t legal_mpp(uint64_t value, uint64_t old) {
  uint64_t mpp = value & kMstatusMpp;
  if (mpp == (2ull << 11)) {
    return old & kMstatusMpp;
  }
  return mpp;
}

uint64_t write_masked(uint64_t old, uint64_t value, uint64_t mask) {
  return (old & ~mask) | (value & mask);
}

void dirty_fp(IdealHart* hart) {
  hart->mstatus = (hart->mstatus & ~kMstatusFs) | kMstatusFsDirty;
}

bool fp_on(const IdealHart* hart) {
  return (hart->mstatus & kMstatusFs) != 0;
}

uint64_t pmp_cfg_read(const IdealHart* hart, unsigned first) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; i++) {
    unsigned index = first + i;
    if (index < kPmpEntries) {
      value |= static_cast<uint64_t>(hart->pmpcfg[index]) << (8u * i);
    }
  }
  return value;
}

void pmp_cfg_write(IdealHart* hart, unsigned first, uint64_t value) {
  constexpr uint64_t kKeep = 0x9fu;
  for (unsigned i = 0; i < 8; i++) {
    unsigned index = first + i;
    if (index >= kPmpEntries || (hart->pmpcfg[index] & 0x80u) != 0) {
      continue;
    }
    uint8_t cfg = static_cast<uint8_t>((value >> (8u * i)) & kKeep);
    if ((cfg & 1u) == 0) {
      cfg = static_cast<uint8_t>(cfg & ~2u);
    }
    hart->pmpcfg[index] = cfg;
  }
}

bool pmp_addr_locked(const IdealHart* hart, unsigned index) {
  if ((hart->pmpcfg[index] & 0x80u) != 0) {
    return true;
  }
  if (index + 1 < kPmpEntries &&
      (hart->pmpcfg[index + 1] & 0x98u) == 0x88u) {
    return true;
  }
  return false;
}

bool sstc_active(const IdealHart* hart) {
  return (hart->menvcfg & kMenvcfgStce) != 0;
}

// M-mode can always touch stimecmp. S-mode only after menvcfg.STCE is set.
bool stimecmp_ok(const IdealHart* hart) {
  return hart->priv == 3 || sstc_active(hart);
}

bool counter_ok(const IdealHart* hart, unsigned index) {
  if (hart->priv >= 3) {
    return true;
  }
  uint64_t bit = 1ull << index;
  if ((hart->mcounteren & bit) == 0) {
    return false;
  }
  if (hart->priv == 0 && (hart->scounteren & bit) == 0) {
    return false;
  }
  return true;
}

uint64_t tvec_write(uint64_t old, uint64_t value) {
  uint64_t mode = value & 3ull;
  if (mode > 1) {
    mode = old & 3ull;
  }
  return (value & ~3ull) | mode;
}

bool read_csr(IdealHart* hart, uint64_t csr, bool write, uint64_t* value) {
  if (csr > 0xfff) {
    return false;
  }
  unsigned need = static_cast<unsigned>((csr >> 8) & 3u);
  if (hart->priv < need) {
    return false;
  }
  if (write && ((csr >> 10) & 3u) == 3u) {
    return false;
  }
  if (hart->priv == 1 && (hart->mstatus & kMstatusTvm) != 0 && csr == 0x180) {
    return false;
  }

  switch (csr) {
    case 0x001:
      if (!fp_on(hart)) return false;
      *value = hart->fcsr & 0x1fu;
      return true;
    case 0x002:
      if (!fp_on(hart)) return false;
      *value = (hart->fcsr >> 5) & 7u;
      return true;
    case 0x003:
      if (!fp_on(hart)) return false;
      *value = hart->fcsr & 0xffu;
      return true;
    case 0x100:
      *value = hart->mstatus & (kSstatusWrite | kMstatusUxl);
      if ((hart->mstatus & kMstatusFs) == kMstatusFsDirty) {
        *value |= kMstatusSd;
      }
      return true;
    case 0x104:
      *value = hart->mie & hart->mideleg & kSieMask;
      return true;
    case 0x105:
      *value = hart->stvec;
      return true;
    case 0x106:
      *value = hart->scounteren & kCounterMask;
      return true;
    case 0x10A:
      *value = hart->senvcfg;
      return true;
    case 0x140:
      *value = hart->sscratch;
      return true;
    case 0x141:
      *value = hart->sepc;
      return true;
    case 0x142:
      *value = hart->scause;
      return true;
    case 0x143:
      *value = hart->stval;
      return true;
    case 0x144:
      *value = (hart->mip | ideal_device_mip()) & hart->mideleg & kSieMask;
      return true;
    case 0x14D:
      if (!stimecmp_ok(hart)) return false;
      *value = hart->stimecmp;
      return true;
    case 0x180:
      *value = hart->satp;
      return true;
    case 0x300:
      *value = hart->mstatus & (kMstatusWrite | kMstatusUxl | kMstatusSxl);
      if ((hart->mstatus & kMstatusFs) == kMstatusFsDirty) {
        *value |= kMstatusSd;
      }
      return true;
    case 0x301:
      *value = kMisa;
      return true;
    case 0x302:
      *value = hart->medeleg & kMedelegMask;
      return true;
    case 0x303:
      *value = hart->mideleg & kSieMask;
      return true;
    case 0x304:
      *value = hart->mie & kMieMask;
      return true;
    case 0x305:
      *value = hart->mtvec;
      return true;
    case 0x306:
      *value = hart->mcounteren & kCounterMask;
      return true;
    case 0x30A:
      *value = hart->menvcfg & kMenvcfgMask;
      return true;
    case 0x320:
      *value = hart->mcountinhibit & 0x5ull;
      return true;
    case 0x340:
      *value = hart->mscratch;
      return true;
    case 0x341:
      *value = hart->mepc;
      return true;
    case 0x342:
      *value = hart->mcause;
      return true;
    case 0x343:
      *value = hart->mtval;
      return true;
    case 0x344:
      *value = (hart->mip | ideal_device_mip()) & kMieMask;
      return true;
    case 0xB00:
      *value = hart->mcycle;
      return true;
    case 0xB02:
      *value = hart->minstret;
      return true;
    case 0xC00:
      if (!counter_ok(hart, 0)) return false;
      *value = hart->mcycle;
      return true;
    case 0xC01:
      if (!counter_ok(hart, 1)) return false;
      *value = hart->time;
      return true;
    case 0xC02:
      if (!counter_ok(hart, 2)) return false;
      *value = hart->minstret;
      return true;
    case 0xF11:
    case 0xF12:
    case 0xF13:
    case 0xF14:
    case 0xF15:
      *value = 0;
      return true;
    default:
      break;
  }

  if ((csr & 1ull) == 0 && csr >= 0x3A0 && csr <= 0x3AE) {
    unsigned first = static_cast<unsigned>((csr - 0x3A0) * 4u);
    *value = first < kPmpEntries ? pmp_cfg_read(hart, first) : 0;
    return true;
  }
  if (csr >= 0x3B0 && csr <= 0x3EF) {
    unsigned index = static_cast<unsigned>(csr - 0x3B0);
    *value = index < kPmpEntries ? hart->pmpaddr[index] : 0;
    return true;
  }
  return false;
}

bool write_csr(IdealHart* hart, uint64_t csr, uint64_t value) {
  uint64_t ignored = 0;
  if (!read_csr(hart, csr, true, &ignored)) {
    return false;
  }
  switch (csr) {
    case 0x001:
      hart->fcsr = (hart->fcsr & ~0x1full) | (value & 0x1fu);
      dirty_fp(hart);
      return true;
    case 0x002:
      hart->fcsr = (hart->fcsr & ~0xe0ull) | ((value & 7u) << 5);
      dirty_fp(hart);
      return true;
    case 0x003:
      hart->fcsr = value & 0xffu;
      dirty_fp(hart);
      return true;
    case 0x100: {
      uint64_t merged = write_masked(hart->mstatus, value, kSstatusWrite);
      hart->mstatus = merged;
      return true;
    }
    case 0x104: {
      uint64_t mask = hart->mideleg & kSieMask;
      hart->mie = (hart->mie & ~mask) | (value & mask);
      return true;
    }
    case 0x105:
      hart->stvec = tvec_write(hart->stvec, value);
      return true;
    case 0x106:
      hart->scounteren = value & kCounterMask;
      return true;
    case 0x10A:
      hart->senvcfg = value;
      return true;
    case 0x140:
      hart->sscratch = value;
      return true;
    case 0x141:
      hart->sepc = value & ~1ull;
      return true;
    case 0x142:
      hart->scause = value;
      return true;
    case 0x143:
      hart->stval = value;
      return true;
    case 0x144: {
      uint64_t mask = hart->mideleg & (1ull << 1);
      if (!sstc_active(hart)) {
        mask |= hart->mideleg & kStip;
      }
      hart->mip = (hart->mip & ~mask) | (value & mask);
      return true;
    }
    case 0x14D:
      if (!stimecmp_ok(hart)) return false;
      hart->stimecmp = value;
      return true;
    case 0x180: {
      uint64_t mode = value & (0xfull << 60);
      if (mode != 0 && mode != (8ull << 60) && mode != (9ull << 60) &&
          mode != (10ull << 60)) {
        mode = hart->satp & (0xfull << 60);
      }
      hart->satp = mode | (value & (((1ull << 16) - 1ull) << 44)) |
                   (value & ((1ull << 44) - 1ull));
      return true;
    }
    case 0x300: {
      uint64_t merged = write_masked(hart->mstatus, value, kMstatusWrite);
      merged = (merged & ~kMstatusMpp) | legal_mpp(merged, hart->mstatus);
      hart->mstatus = merged;
      return true;
    }
    case 0x301:
      return true;
    case 0x302:
      hart->medeleg = value & kMedelegMask;
      return true;
    case 0x303:
      hart->mideleg = value & kSieMask;
      return true;
    case 0x304:
      hart->mie = value & kMieMask;
      return true;
    case 0x305:
      hart->mtvec = tvec_write(hart->mtvec, value);
      return true;
    case 0x306:
      hart->mcounteren = value & kCounterMask;
      return true;
    case 0x30A:
      hart->menvcfg = value & kMenvcfgMask;
      return true;
    case 0x320:
      hart->mcountinhibit = value & 0x5ull;
      return true;
    case 0x340:
      hart->mscratch = value;
      return true;
    case 0x341:
      hart->mepc = value & ~1ull;
      return true;
    case 0x342:
      hart->mcause = value;
      return true;
    case 0x343:
      hart->mtval = value;
      return true;
    case 0x344: {
      uint64_t mask = kSieMask;
      if (sstc_active(hart)) {
        mask &= ~kStip;
      }
      hart->mip = (hart->mip & ~mask) | (value & mask);
      return true;
    }
    case 0xB00:
      hart->mcycle = value;
      return true;
    case 0xB02:
      hart->minstret = value;
      return true;
    case 0xF11:
    case 0xF12:
    case 0xF13:
    case 0xF14:
    case 0xF15:
      return true;
    default:
      break;
  }
  if ((csr & 1ull) == 0 && csr >= 0x3A0 && csr <= 0x3AE) {
    unsigned first = static_cast<unsigned>((csr - 0x3A0) * 4u);
    if (first < kPmpEntries) {
      pmp_cfg_write(hart, first, value);
    }
    return true;
  }
  if (csr >= 0x3B0 && csr <= 0x3EF) {
    unsigned index = static_cast<unsigned>(csr - 0x3B0);
    if (index < kPmpEntries && !pmp_addr_locked(hart, index)) {
      hart->pmpaddr[index] = value & kPmpAddrMask;
    }
    return true;
  }
  return false;
}

}  // namespace

void ideal_fix_sd(IdealHart* hart) {
  if ((hart->mstatus & kMstatusFs) == kMstatusFsDirty) {
    hart->mstatus |= kMstatusSd;
  } else {
    hart->mstatus &= ~kMstatusSd;
  }
}

void ideal_count(IdealHart* hart, bool retired) {
  hart->time += 1;
  if ((hart->mcountinhibit & 1ull) == 0) {
    hart->mcycle += 1;
  }
  if (retired && (hart->mcountinhibit & 4ull) == 0) {
    hart->minstret += 1;
  }
}

void ideal_update_timer(IdealHart* hart) {
  if (!sstc_active(hart)) {
    return;
  }
  if (hart->time >= hart->stimecmp) {
    hart->mip |= kStip;
  } else {
    hart->mip &= ~kStip;
  }
}

uint64_t ideal_pending_interrupt(const IdealHart* hart) {
  uint64_t pending = (hart->mip | ideal_device_mip()) & hart->mie;
  uint64_t eligible = 0;
  if (hart->priv < 3 ||
      (hart->priv == 3 && (hart->mstatus & kMstatusMie) != 0)) {
    eligible = pending & ~hart->mideleg;
  }
  if (eligible == 0 &&
      (hart->priv < 1 ||
       (hart->priv == 1 && (hart->mstatus & kMstatusSie) != 0))) {
    eligible = pending & hart->mideleg;
  }
  static const int kOrder[] = {11, 3, 7, 9, 1, 5};
  for (int line : kOrder) {
    if ((eligible & (1ull << line)) != 0) {
      return (1ull << 63) | static_cast<uint64_t>(line);
    }
  }
  return 0;
}

void ideal_raise(IdealContext* ctx, uint64_t cause, uint64_t tval) {
  IdealHart* hart = ctx->hart;
  bool interrupt = (cause >> 63) != 0;
  uint64_t code = cause & ((1ull << 63) - 1ull);
  uint64_t deleg = interrupt ? hart->mideleg : hart->medeleg;
  bool to_s = hart->priv <= 1 && code < 64 && ((deleg >> code) & 1ull) != 0;
  uint64_t status = hart->mstatus;
  uint64_t vector = 0;
  if (to_s) {
    hart->sepc = hart->pc & ~1ull;
    hart->scause = cause;
    hart->stval = tval;
    uint64_t spie = (status & kMstatusSie) != 0 ? kMstatusSpie : 0;
    status &= ~(kMstatusSpp | kMstatusSpie | kMstatusSie);
    if (hart->priv == 1) {
      status |= kMstatusSpp;
    }
    status |= spie;
    hart->priv = 1;
    vector = hart->stvec;
  } else {
    hart->mepc = hart->pc & ~1ull;
    hart->mcause = cause;
    hart->mtval = tval;
    uint64_t pie = (status & kMstatusMie) != 0 ? kMstatusMpie : 0;
    status &= ~(kMstatusMpp | kMstatusMpie | kMstatusMie);
    status |= (static_cast<uint64_t>(hart->priv) << 11) | pie;
    hart->priv = 3;
    vector = hart->mtvec;
  }
  hart->mstatus = status;
  uint64_t target = vector & ~3ull;
  if (interrupt && (vector & 3ull) == 1ull) {
    target += 4ull * code;
  }
  ctx->npc = target;
  ctx->trapped = true;
}

int ideal_csr(IdealContext* ctx, int op, bool imm) {
  uint64_t csr = ctx->operands.imm;
  uint64_t src = imm ? ctx->operands.rs1
                     : ideal_gpr_read(ctx->hart, ctx->operands.rs1);
  bool writing = op == 0 || src != 0;
  uint64_t old = 0;
  if (!read_csr(ctx->hart, csr, writing, &old)) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
    return STATE_RUNNING;
  }
  if (writing) {
    uint64_t next = src;
    if (op == 1) {
      next = old | src;
    } else if (op == 2) {
      next = old & ~src;
    }
    if (!write_csr(ctx->hart, csr, next)) {
      ideal_raise(ctx, kExcIllegal, ctx->inst);
      return STATE_RUNNING;
    }
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, old);
  return STATE_RUNNING;
}
