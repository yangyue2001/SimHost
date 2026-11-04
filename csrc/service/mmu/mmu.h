#ifndef SIM_SERVICE_MMU_H
#define SIM_SERVICE_MMU_H

#include <cstdint>

#include "service/service.h"

// One translation. The frontend passes the effective privilege: M-mode fetch
// stays M, and a load/store with MPRV set should already pass MPP. sum and
// mxr are the mstatus bits for that privilege.
//
// va_hi is VA[63:57]. vpn4..vpn0 are the five 9-bit indices in VA[56:12].
// page_off is VA[11:0]. Together they are the full RV64 virtual address.
// len is the access size in bytes. pmpcfg0/pmpcfg2 are the RV64 CSRs (eight
// entries each). pmpaddr has 16 entries. kind is kMmuFetch, kMmuLoad, or
// kMmuStore.
constexpr unsigned char kMmuFetch = 0;
constexpr unsigned char kMmuLoad = 1;
constexpr unsigned char kMmuStore = 2;

struct MmuReq {
  unsigned char priv;
  unsigned va_hi;
  unsigned vpn4;
  unsigned vpn3;
  unsigned vpn2;
  unsigned vpn1;
  unsigned vpn0;
  unsigned page_off;
  unsigned len;
  uint64_t satp;
  unsigned char sum;
  unsigned char mxr;
  uint64_t pmpcfg0;
  uint64_t pmpcfg2;
  uint64_t pmpaddr[16];
  unsigned char kind;
};

struct MmuResp {
  uint64_t ppn;
  unsigned char page_fault;
  unsigned char access_fault;
};

// Stateless translation. Verilog calls mmu_req during eval. This service does
// not run on the posedge. It does not share state with the ideal backend.
class IdealMmu : public SimService {
 public:
  IdealMmu();
  ~IdealMmu() override;

  const char* name() const override;
  bool init(const SimContext& context) override;
  void finish(int trap) override;

  MmuResp translate(const MmuReq& req) const;

 private:
  void publish();
  void clear();
};

// Translate with the active IdealMmu. False when that service is not up.
bool mmu_translate(const MmuReq& req, MmuResp* resp);

#endif
