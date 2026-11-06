#ifndef SIM_SERVICE_MMU_H
#define SIM_SERVICE_MMU_H

#include <array>
#include <cstdint>

// One translation context, built on the stack for one DPI or backend call.
// It is not a service: satp, privilege, and PMP belong to this access.
// Privilege, control bits, and satp are checked while building this
// translation. A reserved satp MODE stops. It is not a page fault.

enum class SatpMode : unsigned char {
  kBare = 0,
  kSv39 = 8,
  kSv48 = 9,
  kSv57 = 10,
};

enum class AccessType : unsigned char {
  kFetch = 0,
  kLoad = 1,
  kStore = 2,
};

enum class Priv : unsigned char {
  kUser = 0,
  kSuper = 1,
  kMachine = 3,
};

enum class PmpMatch : unsigned char {
  kOff = 0,
  kTor = 1,
  kNa4 = 2,
  kNapot = 3,
};

struct PmpCfg {
  bool l;
  PmpMatch a;
  bool x;
  bool w;
  bool r;
};

struct PmpEntry {
  PmpCfg cfg;
  uint64_t addr = 0;
};

// Built from one satp CSR. MODE must be Bare, Sv39, Sv48, or Sv57.
// Anything else stops. This class does not report a page fault.
class Satp {
 public:
  explicit Satp(uint64_t raw);

  SatpMode mode() const { return mode_; }
  uint64_t ppn() const { return ppn_; }
  uint16_t asid() const { return asid_; }
  int levels() const;

 private:
  SatpMode mode_;
  uint64_t ppn_;
  uint16_t asid_;
};

class Pmp {
 public:
  static constexpr unsigned kNumEntries = 16;
  static constexpr unsigned kgranule = 0;
  static_assert(kNumEntries == 0 || kNumEntries == 16 || kNumEntries == 64,
                "RISC-V PMP supports only 0, 16, or 64 entries.");

  // cfg packs 8 entries per RV64 pmpcfg word. addr is pmpaddr[0..).
  explicit Pmp(const uint64_t* cfg, const uint64_t* addr);
  // True when [addr, addr + len) may do this access.
  bool check(uint64_t addr, unsigned len, AccessType access, Priv priv) const;

 private:
  std::array<PmpEntry, kNumEntries> entries_{};
  unsigned granule = kgranule;
  unsigned numEntries = kNumEntries;
};

struct MmuReq {
  unsigned char priv;
  unsigned char mpp;
  unsigned char mprv;
  unsigned char sum;
  unsigned char mxr;
  unsigned char kind;
  uint64_t satp;
  uint64_t pmpcfg0;
  uint64_t pmpcfg2;
  uint64_t pmpaddr[16];
  uint64_t vaddr;
};

struct MmuResp {
  uint64_t ppn;
  unsigned char page_fault;
  unsigned char access_fault;
};

// priv, mpp, mprv, sum, mxr, satp, and pmp are fixed for this object.
// translate() is the only entry. walk() stays private so a Bare or M-mode
// access cannot start a page walk.
class Mmu {
 public:
  Mmu(uint8_t priv, uint8_t mpp, uint8_t mprv, uint8_t sum, uint8_t mxr,
      Satp satp, Pmp pmp);
  // mmu 地址翻译
  MmuResp translate(uint64_t vaddr, AccessType access) const;
  // pmp 检查
  bool pmp_ok(uint64_t addr, unsigned len, AccessType access, Priv priv) const;
  Priv effective(AccessType access) const;  // 改变特权级的
 private:
  MmuResp physical(uint64_t addr, AccessType access) const;
  MmuResp fault(unsigned char page, unsigned char access) const;
  MmuResp walk(uint64_t vaddr, AccessType access) const;
  bool read_pte(uint64_t addr, uint64_t* pte) const;

  Priv priv_;
  Priv mpp_;
  bool mprv_;
  bool sum_;
  bool mxr_;
  Satp satp_;
  Pmp pmp_;
};

// Writes resp. A null resp is a caller bug.
void mmu_translate(const MmuReq& req, MmuResp* resp);

#endif
