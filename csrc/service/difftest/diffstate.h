#ifndef SIM_SERVICE_DIFFTEST_DIFFSTATE_H
#define SIM_SERVICE_DIFFTEST_DIFFSTATE_H

#include <cstdint>

// One hart, for one rising edge. DPI-C writes it during eval(). The difftest
// service reads it afterwards and then clears the per-cycle flags.
//
// xrf is the integer file. fpr is the floating-point file. Both are
// snapshots of the whole file, not a write port. Vector state is not here.
// A commit slot writes an integer register when rfwen is set.

constexpr int kDiffCommitWidth = 6;

struct DiffCsr {
  uint64_t privilege;
  uint64_t mstatus;
  uint64_t sstatus;
  uint64_t mepc;
  uint64_t sepc;
  uint64_t mtval;
  uint64_t stval;
  uint64_t mtvec;
  uint64_t stvec;
  uint64_t mcause;
  uint64_t scause;
  uint64_t satp;
  uint64_t mip;
  uint64_t mie;
  uint64_t mscratch;
  uint64_t sscratch;
  uint64_t mideleg;
  uint64_t medeleg;
};

// One retired instruction. valid is 0 when this port retired nothing.
// wdest is the architectural x register, never a physical register.
// rfwen means wdata updates xrf. skip means MMIO: do not exec the reference.
struct DiffCommitSlot {
  unsigned char valid;
  unsigned char rfwen;
  unsigned char skip;
  unsigned char wdest;
  uint64_t pc;
  uint32_t inst;
  uint64_t wdata;
};

struct DiffState {
  DiffCsr csr;
  unsigned char csr_valid;
  uint64_t xrf[32];
  unsigned char xrf_valid;
  uint64_t fpr[32];
  unsigned char fpr_valid;
  DiffCommitSlot commit[kDiffCommitWidth];
  int commit_count;
  unsigned char commit_overflow;
};

const DiffState& diffstate();
void diffstate_clear();
// Drop this cycle's commits and valid flags. Keep the last csr, xrf, and fpr.
void diffstate_retire();

extern "C" {
void difftest_csr(uint64_t privilege, uint64_t mstatus, uint64_t sstatus,
                  uint64_t mepc, uint64_t sepc, uint64_t mtval, uint64_t stval,
                  uint64_t mtvec, uint64_t stvec, uint64_t mcause,
                  uint64_t scause, uint64_t satp, uint64_t mip, uint64_t mie,
                  uint64_t mscratch, uint64_t sscratch, uint64_t mideleg,
                  uint64_t medeleg);
void difftest_xrf(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                  uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
                  uint64_t x8, uint64_t x9, uint64_t x10, uint64_t x11,
                  uint64_t x12, uint64_t x13, uint64_t x14, uint64_t x15,
                  uint64_t x16, uint64_t x17, uint64_t x18, uint64_t x19,
                  uint64_t x20, uint64_t x21, uint64_t x22, uint64_t x23,
                  uint64_t x24, uint64_t x25, uint64_t x26, uint64_t x27,
                  uint64_t x28, uint64_t x29, uint64_t x30, uint64_t x31);
void difftest_fpr(uint64_t f0, uint64_t f1, uint64_t f2, uint64_t f3,
                  uint64_t f4, uint64_t f5, uint64_t f6, uint64_t f7,
                  uint64_t f8, uint64_t f9, uint64_t f10, uint64_t f11,
                  uint64_t f12, uint64_t f13, uint64_t f14, uint64_t f15,
                  uint64_t f16, uint64_t f17, uint64_t f18, uint64_t f19,
                  uint64_t f20, uint64_t f21, uint64_t f22, uint64_t f23,
                  uint64_t f24, uint64_t f25, uint64_t f26, uint64_t f27,
                  uint64_t f28, uint64_t f29, uint64_t f30, uint64_t f31);
// index is the retire port, oldest instruction at 0. valid 0 clears the slot.
void difftest_commit(unsigned char index, unsigned char valid,
                     unsigned char rfwen, unsigned char wdest, uint64_t pc,
                     unsigned int inst, uint64_t wdata, unsigned char skip);
}

#endif
