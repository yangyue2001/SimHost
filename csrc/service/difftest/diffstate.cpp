#include "service/difftest/diffstate.h"

#include <cstring>

namespace {

DiffState state;

void write_commit(int index, unsigned char valid, unsigned char rfwen,
                  unsigned char wdest, uint64_t pc, unsigned int inst,
                  uint64_t wdata, unsigned char skip) {
  DiffCommitSlot& slot = state.commit[index];
  slot.valid = valid;
  slot.rfwen = rfwen;
  slot.skip = skip;
  slot.wdest = wdest;
  slot.pc = pc;
  slot.inst = inst;
  slot.wdata = wdata;
  if (valid && index >= state.commit_count) {
    state.commit_count = index + 1;
  }
}

}  // namespace

const DiffState& diffstate() { return state; }

void diffstate_clear() { std::memset(&state, 0, sizeof(state)); }

void diffstate_retire() {
  std::memset(state.commit, 0, sizeof(state.commit));
  state.commit_count = 0;
  state.commit_overflow = 0;
  state.csr_valid = 0;
  state.xrf_valid = 0;
  state.fpr_valid = 0;
}

extern "C" void difftest_csr(uint64_t privilege, uint64_t mstatus,
                             uint64_t sstatus, uint64_t mepc, uint64_t sepc,
                             uint64_t mtval, uint64_t stval, uint64_t mtvec,
                             uint64_t stvec, uint64_t mcause, uint64_t scause,
                             uint64_t satp, uint64_t mip, uint64_t mie,
                             uint64_t mscratch, uint64_t sscratch,
                             uint64_t mideleg, uint64_t medeleg) {
  state.csr.privilege = privilege;
  state.csr.mstatus = mstatus;
  state.csr.sstatus = sstatus;
  state.csr.mepc = mepc;
  state.csr.sepc = sepc;
  state.csr.mtval = mtval;
  state.csr.stval = stval;
  state.csr.mtvec = mtvec;
  state.csr.stvec = stvec;
  state.csr.mcause = mcause;
  state.csr.scause = scause;
  state.csr.satp = satp;
  state.csr.mip = mip;
  state.csr.mie = mie;
  state.csr.mscratch = mscratch;
  state.csr.sscratch = sscratch;
  state.csr.mideleg = mideleg;
  state.csr.medeleg = medeleg;
  state.csr_valid = 1;
}

extern "C" void difftest_xrf(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                             uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
                             uint64_t x8, uint64_t x9, uint64_t x10,
                             uint64_t x11, uint64_t x12, uint64_t x13,
                             uint64_t x14, uint64_t x15, uint64_t x16,
                             uint64_t x17, uint64_t x18, uint64_t x19,
                             uint64_t x20, uint64_t x21, uint64_t x22,
                             uint64_t x23, uint64_t x24, uint64_t x25,
                             uint64_t x26, uint64_t x27, uint64_t x28,
                             uint64_t x29, uint64_t x30, uint64_t x31) {
  uint64_t value[32] = {x0,  x1,  x2,  x3,  x4,  x5,  x6,  x7,  x8,  x9,  x10,
                        x11, x12, x13, x14, x15, x16, x17, x18, x19, x20, x21,
                        x22, x23, x24, x25, x26, x27, x28, x29, x30, x31};
  for (int index = 0; index < 32; ++index) {
    state.xrf[index] = value[index];
  }
  state.xrf_valid = 1;
}

extern "C" void difftest_fpr(uint64_t f0, uint64_t f1, uint64_t f2, uint64_t f3,
                             uint64_t f4, uint64_t f5, uint64_t f6, uint64_t f7,
                             uint64_t f8, uint64_t f9, uint64_t f10,
                             uint64_t f11, uint64_t f12, uint64_t f13,
                             uint64_t f14, uint64_t f15, uint64_t f16,
                             uint64_t f17, uint64_t f18, uint64_t f19,
                             uint64_t f20, uint64_t f21, uint64_t f22,
                             uint64_t f23, uint64_t f24, uint64_t f25,
                             uint64_t f26, uint64_t f27, uint64_t f28,
                             uint64_t f29, uint64_t f30, uint64_t f31) {
  uint64_t value[32] = {f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,  f8,  f9,  f10,
                        f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21,
                        f22, f23, f24, f25, f26, f27, f28, f29, f30, f31};
  for (int index = 0; index < 32; ++index) {
    state.fpr[index] = value[index];
  }
  state.fpr_valid = 1;
}

extern "C" void difftest_commit(unsigned char index, unsigned char valid,
                                unsigned char rfwen, unsigned char wdest,
                                uint64_t pc, unsigned int inst, uint64_t wdata,
                                unsigned char skip) {
  if (index >= kDiffCommitWidth) {
    state.commit_overflow = 1;
    return;
  }
  write_commit(index, valid, rfwen, wdest, pc, inst, wdata, skip);
}

