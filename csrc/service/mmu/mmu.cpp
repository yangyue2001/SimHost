#include "service/mmu/mmu.h"

#include <cstdint>

namespace {

constexpr uint64_t kRamBase = 0x80000000ull;
constexpr unsigned kPmpEntries = 16;
constexpr unsigned kPmpAddrBits = 54;

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

constexpr unsigned kPmpRead = 1u << 0;
constexpr unsigned kPmpWrite = 1u << 1;
constexpr unsigned kPmpExec = 1u << 2;
constexpr unsigned kPmpTor = 1u;
constexpr unsigned kPmpNa4 = 2u;
constexpr unsigned kPmpNapot = 3u;
constexpr unsigned kPmpLock = 1u << 7;

constexpr uint64_t kSatpBare = 0ull;
constexpr uint64_t kSatpSv39 = 8ull;
constexpr uint64_t kSatpSv48 = 9ull;
constexpr uint64_t kSatpSv57 = 10ull;
constexpr unsigned kVpnBits = 9;

IdealMmu* current_mmu = nullptr;

extern "C" uint64_t memory_read(uint64_t offset);
extern "C" unsigned char memory_covers(uint64_t offset, uint64_t len);
extern "C" void mmu_resp(uint64_t ppn, unsigned char page_fault,
                         unsigned char access_fault);

MmuResp fault(unsigned char page, unsigned char access) {
  MmuResp resp;
  resp.ppn = 0;
  resp.page_fault = page;
  resp.access_fault = access;
  return resp;
}

unsigned char pmp_cfg(const MmuReq& req, unsigned index) {
  uint64_t word = index < 8 ? req.pmpcfg0 : req.pmpcfg2;
  return static_cast<unsigned char>((word >> ((index & 7u) * 8u)) & 0xffu);
}

uint64_t pmp_addr(const MmuReq& req, unsigned index) {
  return req.pmpaddr[index] & ((1ull << kPmpAddrBits) - 1);
}

unsigned pmp_need(unsigned char kind) {
  if (kind == kMmuFetch) {
    return kPmpExec;
  }
  if (kind == kMmuLoad) {
    return kPmpRead;
  }
  return kPmpWrite;
}

// Lowest matching entry wins. A partial overlap is a fault. No match allows
// only M-mode. An unlocked M-mode match allows the access.
bool pmp_ok(const MmuReq& req, uint64_t addr, uint64_t len, unsigned char kind,
            unsigned char priv) {
  if (len == 0 || addr > ~uint64_t{0} - (len - 1)) {
    return false;
  }
  uint64_t end = addr + len;
  for (unsigned i = 0; i < kPmpEntries; i++) {
    unsigned char cfg = pmp_cfg(req, i);
    unsigned mode = (cfg >> 3) & 3u;
    if (mode == 0) {
      continue;
    }
    uint64_t low = 0;
    uint64_t high = 0;
    if (mode == kPmpTor) {
      low = i == 0 ? 0 : pmp_addr(req, i - 1) << 2;
      high = pmp_addr(req, i) << 2;
      if (low >= high) {
        continue;
      }
    } else if (mode == kPmpNa4) {
      low = pmp_addr(req, i) << 2;
      high = low + 4;
    } else if (mode == kPmpNapot) {
      uint64_t addr_i = pmp_addr(req, i);
      unsigned ones = static_cast<unsigned>(
          __builtin_ctzll(~addr_i | (1ull << kPmpAddrBits)));
      low = (addr_i & ~((1ull << ones) - 1)) << 2;
      high = low + (1ull << (ones + 3));
    } else {
      continue;
    }
    if (addr >= high || end <= low) {
      continue;
    }
    if (addr < low || end > high) {
      return false;
    }
    if (priv == 3 && (cfg & kPmpLock) == 0) {
      return true;
    }
    return (cfg & pmp_need(kind)) != 0;
  }
  return priv == 3;
}

bool pa_ok(uint64_t addr, uint64_t len) {
  const uint64_t limit = 1ull << 56;
  return addr < limit && len <= limit - addr;
}

bool in_ram(uint64_t addr, uint64_t len) {
  if (!pa_ok(addr, len) || addr < kRamBase) {
    return false;
  }
  return memory_covers(addr - kRamBase, len) != 0;
}

// A failed page-table read is an access fault of the original access.
bool read_pte(const MmuReq& req, uint64_t addr, uint64_t* pte) {
  if (!pmp_ok(req, addr, 8, kMmuLoad, 1) || !in_ram(addr, 8)) {
    return false;
  }
  *pte = memory_read(addr - kRamBase);
  return true;
}

// va_hi and the five VPN fields are the whole RV64 virtual address.
uint64_t vaddr_of(const MmuReq& req) {
  uint64_t vpn = (static_cast<uint64_t>(req.vpn4 & 0x1ffu) << 36) |
                 (static_cast<uint64_t>(req.vpn3 & 0x1ffu) << 27) |
                 (static_cast<uint64_t>(req.vpn2 & 0x1ffu) << 18) |
                 (static_cast<uint64_t>(req.vpn1 & 0x1ffu) << 9) |
                 static_cast<uint64_t>(req.vpn0 & 0x1ffu);
  return (static_cast<uint64_t>(req.va_hi & 0x7fu) << 57) | (vpn << 12) |
         (req.page_off & 0xfffu);
}

// Same selection as ISS: 8/9/10 are Sv39/Sv48/Sv57. Anything else does not
// walk.
int paging_levels(uint64_t satp) {
  uint64_t mode = (satp >> 60) & 0xfull;
  if (mode == kSatpSv39) {
    return 3;
  }
  if (mode == kSatpSv48) {
    return 4;
  }
  if (mode == kSatpSv57) {
    return 5;
  }
  return 0;
}

// ISS rejects a non-canonical address before the walk. Bits above `bits` must
// all equal bit `bits - 1`.
bool canonical(uint64_t vaddr, int bits) {
  uint64_t sign = (vaddr >> (bits - 1)) & 1ull;
  uint64_t ones = (1ull << (64 - bits)) - 1;
  return (vaddr >> bits) == (sign != 0 ? ones : 0);
}

bool legal_len(unsigned len) {
  return len == 1 || len == 2 || len == 4 || len == 8;
}

MmuResp physical(const MmuReq& req, uint64_t addr) {
  // RAM membership is not an access fault here. The frontend uses ppn for
  // the later memory access. PMP and the 56-bit physical address are.
  if (!pa_ok(addr, req.len) ||
      !pmp_ok(req, addr, req.len, req.kind, req.priv)) {
    return fault(0, 1);
  }
  MmuResp resp;
  resp.ppn = (addr >> 12) & kPpnMask;
  resp.page_fault = 0;
  resp.access_fault = 0;
  return resp;
}

MmuResp translate_walk(const MmuReq& req, uint64_t vaddr, int levels) {
  uint64_t table = (req.satp & kPpnMask) << 12;
  for (int level = levels - 1; level >= 0; level--) {
    unsigned shift = 12u + static_cast<unsigned>(level) * kVpnBits;
    uint64_t vpn = (vaddr >> shift) & 0x1ffu;
    uint64_t pte_addr = table + vpn * 8;
    uint64_t pte = 0;
    if (!read_pte(req, pte_addr, &pte)) {
      return fault(0, 1);
    }
    bool reserved = (pte & kPteV) == 0 ||
                    ((pte & kPteR) == 0 && (pte & kPteW) != 0) ||
                    (pte & (kPteReserved | kPtePbmt | kPteN)) != 0;
    if (reserved) {
      return fault(1, 0);
    }
    uint64_t ppn = (pte >> 10) & kPpnMask;
    if ((pte & (kPteR | kPteX)) == 0) {
      if (level == 0 || (pte & (kPteU | kPteA | kPteD)) != 0) {
        return fault(1, 0);
      }
      table = ppn << 12;
      continue;
    }

    bool user_page = (pte & kPteU) != 0;
    if (req.priv == 0) {
      if (!user_page) {
        return fault(1, 0);
      }
    } else if (user_page && (req.kind == kMmuFetch || req.sum == 0)) {
      return fault(1, 0);
    }

    bool permitted = false;
    if (req.kind == kMmuFetch) {
      permitted = (pte & kPteX) != 0;
    } else if (req.kind == kMmuLoad) {
      permitted = (pte & kPteR) != 0 || (req.mxr != 0 && (pte & kPteX) != 0);
    } else if (req.kind == kMmuStore) {
      permitted = (pte & kPteW) != 0;
    }
    if (!permitted) {
      return fault(1, 0);
    }

    unsigned dropped = static_cast<unsigned>(level) * kVpnBits;
    uint64_t low_mask = dropped == 0 ? 0 : (1ull << dropped) - 1;
    if ((ppn & low_mask) != 0) {
      return fault(1, 0);
    }
    bool writes = req.kind == kMmuStore;
    uint64_t needed = kPteA | (writes ? kPteD : 0);
    if ((pte & needed) != needed) {
      return fault(1, 0);
    }

    // Superpage: the skipped low PPN bits come from the virtual address.
    uint64_t full = (ppn | ((vaddr >> 12) & low_mask)) << 12;
    return physical(req, full | (vaddr & 0xfffu));
  }
  return fault(1, 0);
}

}  // namespace

IdealMmu::IdealMmu() = default;

IdealMmu::~IdealMmu() { clear(); }

const char* IdealMmu::name() const { return "mmu"; }

void IdealMmu::publish() { current_mmu = this; }

void IdealMmu::clear() {
  if (current_mmu == this) {
    current_mmu = nullptr;
  }
}

bool IdealMmu::init(const SimContext&) {
  if (current_mmu != nullptr && current_mmu != this) {
    return false;
  }
  publish();
  return true;
}

void IdealMmu::finish(int) { clear(); }

MmuResp IdealMmu::translate(const MmuReq& req) const {
  if (req.priv != 0 && req.priv != 1 && req.priv != 3) {
    return fault(0, 1);
  }
  if (req.kind != kMmuFetch && req.kind != kMmuLoad && req.kind != kMmuStore) {
    return fault(0, 1);
  }
  if (!legal_len(req.len) || req.page_off > 0xfffu ||
      req.page_off + req.len > 0x1000u) {
    return fault(0, 1);
  }

  // M-mode does not consult satp. Bare does not walk. The address is used as
  // given; nothing is sign-extended from VA[38].
  uint64_t mode = (req.satp >> 60) & 0xfull;
  uint64_t vaddr = vaddr_of(req);
  if (req.priv == 3 || mode == kSatpBare) {
    return physical(req, vaddr);
  }
  int levels = paging_levels(req.satp);
  if (levels == 0 || !canonical(vaddr, 12 + levels * 9)) {
    return fault(1, 0);
  }
  return translate_walk(req, vaddr, levels);
}

extern "C" void mmu_req(
    unsigned char priv, unsigned int va_hi, unsigned int vpn4,
    unsigned int vpn3, unsigned int vpn2, unsigned int vpn1, unsigned int vpn0,
    unsigned int page_off, unsigned int len, uint64_t satp, unsigned char sum,
    unsigned char mxr, uint64_t pmpcfg0, uint64_t pmpcfg2, uint64_t pmpaddr0,
    uint64_t pmpaddr1, uint64_t pmpaddr2, uint64_t pmpaddr3, uint64_t pmpaddr4,
    uint64_t pmpaddr5, uint64_t pmpaddr6, uint64_t pmpaddr7, uint64_t pmpaddr8,
    uint64_t pmpaddr9, uint64_t pmpaddr10, uint64_t pmpaddr11,
    uint64_t pmpaddr12, uint64_t pmpaddr13, uint64_t pmpaddr14,
    uint64_t pmpaddr15, unsigned char kind) {
  if (current_mmu == nullptr) {
    mmu_resp(0, 0, 1);
    return;
  }
  MmuReq req;
  req.priv = priv;
  req.va_hi = va_hi;
  req.vpn4 = vpn4;
  req.vpn3 = vpn3;
  req.vpn2 = vpn2;
  req.vpn1 = vpn1;
  req.vpn0 = vpn0;
  req.page_off = page_off;
  req.len = len;
  req.satp = satp;
  req.sum = sum;
  req.mxr = mxr;
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
  req.kind = kind;
  MmuResp resp = current_mmu->translate(req);
  mmu_resp(resp.ppn, resp.page_fault, resp.access_fault);
}

bool mmu_translate(const MmuReq& req, MmuResp* resp) {
  if (resp == nullptr) {
    return false;
  }
  if (current_mmu == nullptr) {
    *resp = fault(0, 1);
    return false;
  }
  *resp = current_mmu->translate(req);
  return true;
}
