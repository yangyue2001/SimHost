#ifndef SIM_SERVICE_IDEAL_H
#define SIM_SERVICE_IDEAL_H

#include <cstdint>

#include "service/ideal/exec.h"
#include "service/service.h"

// One instruction, executed in the DPI call. pc is the instruction address.
// page_fault and access_fault are fetch faults. A pc other than the
// architectural pc does not execute. ideal_backend_resp then publishes the
// whole hart: 32 GPRs, 32 FPRs, the CSRs this hart stores, and, for a retired
// conditional branch, whether it was taken and its next pc. mmio is 1 when
// this instruction touched the UART, CLINT, or PLIC window. The other mmio
// fields describe that one access. RAM leaves them 0.
//
// There is no queue and no on_posedge work. Data accesses are translated by
// IdealMmu before they reach RAM or a device window. A byte store to
// 0x10000000 prints. CLINT and PLIC reads are zero. A pending, enabled
// interrupt is taken before the instruction. Verilog exports
// ideal_backend_resp; the weak definition is empty.
extern "C" void ideal_backend_resp(
    uint64_t gpr0,
    uint64_t gpr1,
    uint64_t gpr2,
    uint64_t gpr3,
    uint64_t gpr4,
    uint64_t gpr5,
    uint64_t gpr6,
    uint64_t gpr7,
    uint64_t gpr8,
    uint64_t gpr9,
    uint64_t gpr10,
    uint64_t gpr11,
    uint64_t gpr12,
    uint64_t gpr13,
    uint64_t gpr14,
    uint64_t gpr15,
    uint64_t gpr16,
    uint64_t gpr17,
    uint64_t gpr18,
    uint64_t gpr19,
    uint64_t gpr20,
    uint64_t gpr21,
    uint64_t gpr22,
    uint64_t gpr23,
    uint64_t gpr24,
    uint64_t gpr25,
    uint64_t gpr26,
    uint64_t gpr27,
    uint64_t gpr28,
    uint64_t gpr29,
    uint64_t gpr30,
    uint64_t gpr31,
    uint64_t fpr0,
    uint64_t fpr1,
    uint64_t fpr2,
    uint64_t fpr3,
    uint64_t fpr4,
    uint64_t fpr5,
    uint64_t fpr6,
    uint64_t fpr7,
    uint64_t fpr8,
    uint64_t fpr9,
    uint64_t fpr10,
    uint64_t fpr11,
    uint64_t fpr12,
    uint64_t fpr13,
    uint64_t fpr14,
    uint64_t fpr15,
    uint64_t fpr16,
    uint64_t fpr17,
    uint64_t fpr18,
    uint64_t fpr19,
    uint64_t fpr20,
    uint64_t fpr21,
    uint64_t fpr22,
    uint64_t fpr23,
    uint64_t fpr24,
    uint64_t fpr25,
    uint64_t fpr26,
    uint64_t fpr27,
    uint64_t fpr28,
    uint64_t fpr29,
    uint64_t fpr30,
    uint64_t fpr31,
    uint64_t pc,
    unsigned char priv,
    uint64_t satp,
    uint64_t mstatus,
    uint64_t mepc,
    uint64_t mtvec,
    uint64_t mcause,
    uint64_t mtval,
    unsigned char branch,
    unsigned char taken,
    uint64_t target,
    unsigned char mmio,
    unsigned char mmio_store,
    unsigned char mmio_size,
    uint64_t mmio_addr,
    uint64_t mmio_data
);

// Verilog calls this during eval. The instruction runs before the call returns.
extern "C" void ideal_backend_inst(uint64_t pc, unsigned int inst,
                                   unsigned char page_fault,
                                   unsigned char access_fault);

// Software RV64GC hart. Verilog calls ideal_backend_inst during eval.
class IdealBackend : public SimService {
 public:
  IdealBackend();
  ~IdealBackend() override;

  const char* name() const override;
  bool init(const SimContext& context) override;
  void finish(int trap) override;

  void step(uint64_t pc, uint32_t inst, unsigned char page_fault,
            unsigned char access_fault);
  IdealHart* hart();

 private:
  void reset();
  void publish(unsigned char branch, unsigned char taken, uint64_t target,
               const IdealMmio& mmio) const;

  IdealHart hart_;
  uint64_t executed_;
};

#endif

// The hart currently published by IdealBackend::init. Null before init.
IdealHart* ideal_hart();
