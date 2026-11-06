#include "service/mmu/mmu.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "bits.h"

namespace {

constexpr uint64_t kRamBase = 0x80000000ull;

constexpr uint64_t kPteV = 1ull << 0;
constexpr uint64_t kPteR = 1ull << 1;
constexpr uint64_t kPteW = 1ull << 2;
constexpr uint64_t kPteX = 1ull << 3;
constexpr uint64_t kPteU = 1ull << 4;
constexpr uint64_t kPteA = 1ull << 6;
constexpr uint64_t kPteD = 1ull << 7;
constexpr uint64_t kPteReserved = ((1ull << 7) - 1) << 54;
constexpr uint64_t kPtePbmt = 3ull << 61;
constexpr uint64_t kPteN = 1ull << 63;
constexpr uint64_t kPpnMask = (1ull << 44) - 1;

constexpr unsigned kVpnBits = 9;

extern "C" uint64_t memory_read(uint64_t offset);
extern "C" void mmu_resp(uint64_t ppn, uint8_t page_fault,
                         uint8_t access_fault);

void require_priv(uint8_t raw, Priv* priv) {
  switch (raw) {
    case static_cast<uint8_t>(Priv::kUser):
    case static_cast<uint8_t>(Priv::kSuper):
    case static_cast<uint8_t>(Priv::kMachine):
      *priv = static_cast<Priv>(raw);
      return;
    default:
      std::fprintf(stderr, "mmu priv %u is not 0, 1, or 3\n", raw);
      std::abort();
  }
}

void require_access(uint8_t raw, AccessType* access) {
  switch (raw) {
    case static_cast<uint8_t>(AccessType::kFetch):
    case static_cast<uint8_t>(AccessType::kLoad):
    case static_cast<uint8_t>(AccessType::kStore):
      *access = static_cast<AccessType>(raw);
      return;
    default:
      std::fprintf(stderr, "mmu access %u is not fetch, load, or store\n", raw);
      std::abort();
  }
}

bool require_bit(uint8_t raw, const char* name) {
  if (raw > 1) {
    std::fprintf(stderr, "mmu %s %u is not 0 or 1\n", name, raw);
    std::abort();
  }
  return raw != 0;
}

bool canonical(uint64_t vaddr, int bits) {
  uint64_t sign = (vaddr >> (bits - 1)) & 1ull;
  uint64_t ones = (1ull << (64 - bits)) - 1;
  return (vaddr >> bits) == (sign != 0 ? ones : 0);
}

}  // namespace

Satp::Satp(uint64_t raw) {
  switch (bits<60, 63>(raw)) {
    case static_cast<unsigned>(SatpMode::kBare):
      mode_ = SatpMode::kBare;
      break;
    case static_cast<unsigned>(SatpMode::kSv39):
      mode_ = SatpMode::kSv39;
      break;
    case static_cast<unsigned>(SatpMode::kSv48):
      mode_ = SatpMode::kSv48;
      break;
    case static_cast<unsigned>(SatpMode::kSv57):
      mode_ = SatpMode::kSv57;
      break;
    default:
      std::fprintf(stderr, "satp mode %u is not bare, sv39, sv48, or sv57\n",
                   static_cast<unsigned>(bits<60, 63>(raw)));
      std::abort();
  }
  ppn_ = bits<0, 43>(raw);
  asid_ = static_cast<uint16_t>(bits<44, 59>(raw));
}

int Satp::levels() const {
  switch (mode_) {
    case SatpMode::kSv39:
      return 3;
    case SatpMode::kSv48:
      return 4;
    case SatpMode::kSv57:
      return 5;
    case SatpMode::kBare:
      return 0;
  }
  return 0;
}

Pmp::Pmp(const uint64_t* cfg, const uint64_t* addr) {
  constexpr uint64_t kAddrMask = (1ull << 54) - 1;
  for (unsigned i = 0; i < kNumEntries; i++) {
    uint64_t word = cfg[i / 8];
    unsigned shift = (i % 8) * 8u;
    uint8_t raw = static_cast<uint8_t>((word >> shift) & 0xffu);
    raw = static_cast<uint8_t>(raw & 0x9fu);
    if ((raw & 1u) == 0) {
      raw = static_cast<uint8_t>(raw & ~2u);
    }
    PmpMatch match = static_cast<PmpMatch>((raw >> 3) & 0x03u);
    // G == 0 is 4-byte grain and may be NA4. A coarser grain cannot.
    if (granule != 0 && match == PmpMatch::kNa4) {
      match = PmpMatch::kOff;
    }
    uint64_t pmpaddr = (addr[i] & kAddrMask);
    if (granule != 0 && (match == PmpMatch::kOff || match == PmpMatch::kTor)) {
      pmpaddr &= ~((uint64_t{1} << granule) - 1);
    }
    if (granule >= 2 && match == PmpMatch::kNapot) {
      pmpaddr |= (uint64_t{1} << (granule - 1)) - 1;
      pmpaddr &= kAddrMask;
    }

    entries_[i].cfg.r = (raw & 0x01u) != 0;
    entries_[i].cfg.w = (raw & 0x02u) != 0;
    entries_[i].cfg.x = (raw & 0x04u) != 0;
    entries_[i].cfg.a = match;
    entries_[i].cfg.l = (raw & 0x80u) != 0;
    entries_[i].addr = pmpaddr;
  }
}

bool Pmp::check(uint64_t addr, unsigned len, AccessType access,
                Priv priv) const {
  if (len == 0 || addr > ~uint64_t{0} - (len - 1u)) {
    return false;
  }
  if (numEntries == 0) {
    return true;
  }

  uint64_t start = addr;
  uint64_t end = addr + len;
  uint64_t prev_pmpaddr = 0;
  uint64_t tor_mask =
      granule == 0 ? ~uint64_t{0} : ~((uint64_t{1} << granule) - 1);
  uint64_t napot_fill = granule == 0 ? 0 : (uint64_t{1} << granule) - 1;

  for (unsigned i = 0; i < numEntries; i++) {
    const PmpEntry& item = entries_[i];
    PmpMatch match = item.cfg.a;
    uint64_t low = 0;
    uint64_t high = 0;
    bool covered = false;

    switch (match) {
      case PmpMatch::kOff:
        break;
      case PmpMatch::kTor:
        low = (prev_pmpaddr & tor_mask) << 2;
        high = (item.addr & tor_mask) << 2;
        covered = low < high;
        break;
      case PmpMatch::kNa4:
      case PmpMatch::kNapot: {
        constexpr unsigned kEncodedBits = 55;
        uint64_t encoded = (item.addr << 1) | napot_fill;
        if (match == PmpMatch::kNapot) {
          encoded |= 1u;
        }
        unsigned ones = static_cast<unsigned>(
            __builtin_ctzll(~encoded | (uint64_t{1} << kEncodedBits)));
        uint64_t aligned = encoded & ~((uint64_t{1} << ones) - 1);
        low = (aligned >> 1) << 2;
        high = low + (uint64_t{1} << (ones + 2));
        covered = true;
        break;
      }
    }

    prev_pmpaddr = item.addr;
    if (!covered || end <= low || start >= high) {
      continue;
    }
    if (start < low || end > high) {
      return false;
    }
    if (priv == Priv::kMachine && !item.cfg.l) {
      return true;
    }
    switch (access) {
      case AccessType::kFetch:
        return item.cfg.x;
      case AccessType::kLoad:
        return item.cfg.r;
      default:
        return item.cfg.w;
    }
  }

  return priv == Priv::kMachine;
}

Mmu::Mmu(uint8_t priv, uint8_t mpp, uint8_t mprv, uint8_t sum, uint8_t mxr,
         Satp satp, Pmp pmp)
    : satp_(satp), pmp_(pmp) {
  require_priv(priv, &priv_);
  require_priv(mpp, &mpp_);
  mprv_ = require_bit(mprv, "mprv");
  sum_ = require_bit(sum, "sum");
  mxr_ = require_bit(mxr, "mxr");
}

Priv Mmu::effective(AccessType access) const {
  if (access != AccessType::kFetch && mprv_ && priv_ == Priv::kMachine) {
    return mpp_;
  }
  return priv_;
}

bool Mmu::pmp_ok(uint64_t addr, unsigned len, AccessType access,
                 Priv priv) const {
  return pmp_.check(addr, len, access, priv);
}

bool Mmu::read_pte(uint64_t addr, uint64_t* pte) const {
  if (!pmp_ok(addr, 8, AccessType::kLoad, Priv::kSuper)) {
    return false;
  }
  *pte = memory_read(addr - kRamBase);
  return true;
}

MmuResp Mmu::physical(uint64_t addr, AccessType access) const {
  MmuResp resp;
  resp.ppn = (addr >> 12) & kPpnMask;
  resp.page_fault = 0;
  resp.access_fault = 0;
  return resp;
}

MmuResp Mmu::fault(uint8_t page, uint8_t access) const {
  MmuResp resp;
  resp.ppn = 0;
  resp.page_fault = page;
  resp.access_fault = access;
  return resp;
}

MmuResp Mmu::walk(uint64_t vaddr, AccessType access) const {
  Priv priv = effective(access);
  uint64_t table = satp_.ppn() << 12;
  int levels = satp_.levels();

  for (int level = levels - 1; level >= 0; level--) {
    unsigned shift = 12u + static_cast<unsigned>(level) * kVpnBits;
    uint64_t vpn = (vaddr >> shift) & 0x1ffu;
    uint64_t pte_addr = table + vpn * 8;
    uint64_t pte = 0;
    if (!read_pte(pte_addr, &pte)) {
      return fault(0, 1);
    }
    bool reserved = (pte & kPteV) == 0 ||
                    ((pte & kPteR) == 0 && (pte & kPteW) != 0) ||
                    (pte & (kPteReserved | kPtePbmt | kPteN)) != 0;
    if (reserved) {
      return fault(1, 0);
    }
    uint64_t ppn = (pte >> 10) & kPpnMask;

    // 进行non-leaf pte 的检查 如果是non-leaf pte 的话 跳过
    if ((pte & (kPteR | kPteX)) == 0) {
      if (level == 0 || (pte & (kPteU | kPteA | kPteD)) != 0) {
        return fault(1, 0);
      }
      table = ppn << 12;
      continue;
    }
    // leaf-pte的检查
    bool user_page = (pte & kPteU) != 0;

    if (priv == Priv::kUser) {
      if (!user_page) {
        return fault(1, 0);
      }
    } else if (user_page && (access == AccessType::kFetch || !sum_)) {
      return fault(1, 0);
    }
    // 权限检查
    bool permitted = false;
    if (access == AccessType::kFetch) {
      permitted = (pte & kPteX) != 0;
    } else if (access == AccessType::kLoad) {
      permitted = (pte & kPteR) != 0 || (mxr_ && (pte & kPteX) != 0);
    } else {
      permitted = (pte & kPteW) != 0;
    }
    if (!permitted) {
      return fault(1, 0);
    }
    // ppn 检查。 包括大页检查
    unsigned dropped = static_cast<unsigned>(level) * kVpnBits;
    uint64_t low_mask = dropped == 0 ? 0 : (1ull << dropped) - 1;
    if ((ppn & low_mask) != 0) {
      return fault(1, 0);
    }
    uint64_t needed = kPteA | (access == AccessType::kStore ? kPteD : 0);
    if ((pte & needed) != needed) {
      return fault(1, 0);
    }

    uint64_t full = (ppn | ((vaddr >> 12) & low_mask)) << 12;
    return physical(full | (vaddr & 0xfffu), access);
  }

  return fault(1, 0);
}

MmuResp Mmu::translate(uint64_t vaddr, AccessType access) const {
  Priv priv = effective(access);
  if (priv == Priv::kMachine || satp_.mode() == SatpMode::kBare) {
    return physical(vaddr, access);
  }

  int bits = 12 + satp_.levels() * 9;
  if (!canonical(vaddr, bits)) {
    return fault(1, 0);
  }
  return walk(vaddr, access);
}

void mmu_translate(const MmuReq& req, MmuResp* resp) {
  assert(resp != nullptr);
  AccessType access;
  require_access(req.kind, &access);
  uint64_t cfg[2] = {req.pmpcfg0, req.pmpcfg2};
  Mmu mmu(req.priv, req.mpp, req.mprv, req.sum, req.mxr, Satp(req.satp),
          Pmp(cfg, req.pmpaddr));
  *resp = mmu.translate(req.vaddr, access);
}

extern "C" void mmu_req(uint8_t priv, uint8_t mpp, uint8_t mprv, uint8_t sum,
                        uint8_t mxr, uint8_t kind, uint64_t satp,
                        uint64_t vaddr, uint64_t pmpcfg0, uint64_t pmpcfg2,
                        uint64_t pmpaddr0, uint64_t pmpaddr1, uint64_t pmpaddr2,
                        uint64_t pmpaddr3, uint64_t pmpaddr4, uint64_t pmpaddr5,
                        uint64_t pmpaddr6, uint64_t pmpaddr7, uint64_t pmpaddr8,
                        uint64_t pmpaddr9, uint64_t pmpaddr10,
                        uint64_t pmpaddr11, uint64_t pmpaddr12,
                        uint64_t pmpaddr13, uint64_t pmpaddr14,
                        uint64_t pmpaddr15) {
  MmuReq req{};
  req.priv = priv;
  req.mpp = mpp;
  req.mprv = mprv;
  req.sum = sum;
  req.mxr = mxr;
  req.kind = kind;
  req.vaddr = vaddr;
  req.satp = satp;
  req.pmpcfg0 = pmpcfg0;
  req.pmpcfg2 = pmpcfg2;
  req.pmpaddr[0] = pmpaddr0;
  req.pmpaddr[1] = pmpaddr1;
  req.pmpaddr[2] = pmpaddr2;
  req.pmpaddr[3] = pmpaddr3;
  req.pmpaddr[4] = pmpaddr4;
  req.pmpaddr[5] = pmpaddr5;
  req.pmpaddr[6] = pmpaddr6;
  req.pmpaddr[7] = pmpaddr7;
  req.pmpaddr[8] = pmpaddr8;
  req.pmpaddr[9] = pmpaddr9;
  req.pmpaddr[10] = pmpaddr10;
  req.pmpaddr[11] = pmpaddr11;
  req.pmpaddr[12] = pmpaddr12;
  req.pmpaddr[13] = pmpaddr13;
  req.pmpaddr[14] = pmpaddr14;
  req.pmpaddr[15] = pmpaddr15;
  MmuResp resp{};
  mmu_translate(req, &resp);
  mmu_resp(resp.ppn, resp.page_fault, resp.access_fault);
}

extern "C" void pmp_req(uint8_t priv, uint8_t mpp, uint8_t mprv, uint8_t len,
                        uint8_t kind, uint64_t addr, uint64_t pmpcfg0,
                        uint64_t pmpcfg2, uint64_t pmpaddr0, uint64_t pmpaddr1,
                        uint64_t pmpaddr2, uint64_t pmpaddr3, uint64_t pmpaddr4,
                        uint64_t pmpaddr5, uint64_t pmpaddr6, uint64_t pmpaddr7,
                        uint64_t pmpaddr8, uint64_t pmpaddr9,
                        uint64_t pmpaddr10, uint64_t pmpaddr11,
                        uint64_t pmpaddr12, uint64_t pmpaddr13,
                        uint64_t pmpaddr14, uint64_t pmpaddr15) {
  uint64_t pmpCfg[2] = {pmpcfg0, pmpcfg2};
  uint64_t pmpAddr[16] = {
      pmpaddr0,  pmpaddr1,  pmpaddr2,  pmpaddr3,  pmpaddr4,  pmpaddr5,
      pmpaddr6,  pmpaddr7,  pmpaddr8,  pmpaddr9,  pmpaddr10, pmpaddr11,
      pmpaddr12, pmpaddr13, pmpaddr14, pmpaddr15,
  };
  Mmu mmu(priv, mpp, mprv, 0, 0, Satp(0), Pmp(pmpCfg, pmpAddr));

  AccessType access;
  require_access(kind, &access);

  mmu.pmp_ok(addr, len, access, mmu.effective(access));
}