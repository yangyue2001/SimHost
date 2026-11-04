#include "service/ideal/exec.h"

#include "service/service.h"

#include <cstdint>

namespace {

uint64_t src1(const IdealContext* ctx) {
  return ideal_gpr_read(ctx->hart, ctx->operands.rs1);
}

uint64_t src2(const IdealContext* ctx) {
  return ideal_gpr_read(ctx->hart, ctx->operands.rs2);
}

uint64_t sext32(uint64_t value) {
  return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
}

uint64_t sra64(uint64_t value, unsigned shamt) {
  if (shamt == 0) {
    return value;
  }
  uint64_t result = value >> shamt;
  if ((value >> 63) != 0) {
    result |= ~0ull << (64 - shamt);
  }
  return result;
}

uint32_t sra32(uint32_t value, unsigned shamt) {
  if (shamt == 0) {
    return value;
  }
  uint32_t result = value >> shamt;
  if ((value >> 31) != 0) {
    result |= 0xffffffffu << (32 - shamt);
  }
  return result;
}

int load_ext(IdealContext* ctx, unsigned size, bool sign) {
  uint64_t raw = 0;
  uint64_t addr = src1(ctx) + ctx->operands.imm;
  if (!ideal_load_data(ctx, addr, size, &raw)) {
    return STATE_RUNNING;
  }
  if (sign && size < 8) {
    uint64_t sign_bit = 1ull << (size * 8 - 1);
    uint64_t mask = (1ull << (size * 8)) - 1ull;
    raw &= mask;
    if ((raw & sign_bit) != 0) {
      raw |= ~mask;
    }
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, raw);
  return STATE_RUNNING;
}

int store_len(IdealContext* ctx, unsigned size) {
  uint64_t addr = src1(ctx) + ctx->operands.imm;
  ideal_store_data(ctx, addr, size, src2(ctx));
  return STATE_RUNNING;
}

int branch(IdealContext* ctx, bool taken) {
  if (!taken) {
    return STATE_RUNNING;
  }
  uint64_t target = ctx->hart->pc + ctx->operands.imm;
  if ((target & 1ull) != 0) {
    ideal_raise(ctx, kExcInstMisaligned, target);
    return STATE_RUNNING;
  }
  ctx->npc = target;
  return STATE_RUNNING;
}

}  // namespace

uint64_t ideal_gpr_read(const IdealHart* hart, uint8_t rs) {
  if (rs == 0) {
    return 0;
  }
  return hart->gpr[rs];
}

void ideal_gpr_write(IdealHart* hart, uint8_t rd, uint64_t value) {
  if (rd != 0) {
    hart->gpr[rd] = value;
  }
}

int exec_lui(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, ctx->operands.imm);
  return STATE_RUNNING;
}

int exec_auipc(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, ctx->hart->pc + ctx->operands.imm);
  return STATE_RUNNING;
}

int exec_jal(IdealContext* ctx) {
  uint64_t target = ctx->hart->pc + ctx->operands.imm;
  if ((target & 1ull) != 0) {
    ideal_raise(ctx, kExcInstMisaligned, target);
    return STATE_RUNNING;
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, ctx->npc);
  ctx->npc = target;
  return STATE_RUNNING;
}

int exec_jalr(IdealContext* ctx) {
  uint64_t target = (src1(ctx) + ctx->operands.imm) & ~1ull;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, ctx->npc);
  ctx->npc = target;
  return STATE_RUNNING;
}

int exec_beq(IdealContext* ctx) { return branch(ctx, src1(ctx) == src2(ctx)); }
int exec_bne(IdealContext* ctx) { return branch(ctx, src1(ctx) != src2(ctx)); }
int exec_blt(IdealContext* ctx) {
  return branch(ctx, static_cast<int64_t>(src1(ctx)) <
                         static_cast<int64_t>(src2(ctx)));
}
int exec_bge(IdealContext* ctx) {
  return branch(ctx, static_cast<int64_t>(src1(ctx)) >=
                         static_cast<int64_t>(src2(ctx)));
}
int exec_bltu(IdealContext* ctx) { return branch(ctx, src1(ctx) < src2(ctx)); }
int exec_bgeu(IdealContext* ctx) { return branch(ctx, src1(ctx) >= src2(ctx)); }

int exec_lb(IdealContext* ctx) { return load_ext(ctx, 1, true); }
int exec_lh(IdealContext* ctx) { return load_ext(ctx, 2, true); }
int exec_lw(IdealContext* ctx) { return load_ext(ctx, 4, true); }
int exec_ld(IdealContext* ctx) { return load_ext(ctx, 8, false); }
int exec_lbu(IdealContext* ctx) { return load_ext(ctx, 1, false); }
int exec_lhu(IdealContext* ctx) { return load_ext(ctx, 2, false); }
int exec_lwu(IdealContext* ctx) { return load_ext(ctx, 4, false); }

int exec_sb(IdealContext* ctx) { return store_len(ctx, 1); }
int exec_sh(IdealContext* ctx) { return store_len(ctx, 2); }
int exec_sw(IdealContext* ctx) { return store_len(ctx, 4); }
int exec_sd(IdealContext* ctx) { return store_len(ctx, 8); }

int exec_addi(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) + ctx->operands.imm);
  return STATE_RUNNING;
}
int exec_slti(IdealContext* ctx) {
  uint64_t result = static_cast<int64_t>(src1(ctx)) <
                            static_cast<int64_t>(ctx->operands.imm)
                        ? 1
                        : 0;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, result);
  return STATE_RUNNING;
}
int exec_sltiu(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd,
                  src1(ctx) < ctx->operands.imm ? 1 : 0);
  return STATE_RUNNING;
}
int exec_xori(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) ^ ctx->operands.imm);
  return STATE_RUNNING;
}
int exec_ori(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) | ctx->operands.imm);
  return STATE_RUNNING;
}
int exec_andi(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) & ctx->operands.imm);
  return STATE_RUNNING;
}
int exec_addiw(IdealContext* ctx) {
  uint32_t result =
      static_cast<uint32_t>(src1(ctx)) + static_cast<uint32_t>(ctx->operands.imm);
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_slli(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) << shamt);
  return STATE_RUNNING;
}
int exec_srli(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) >> shamt);
  return STATE_RUNNING;
}
int exec_srai(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sra64(src1(ctx), shamt));
  return STATE_RUNNING;
}
int exec_slliw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x1fu;
  uint32_t result = static_cast<uint32_t>(src1(ctx)) << shamt;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_srliw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x1fu;
  uint32_t result = static_cast<uint32_t>(src1(ctx)) >> shamt;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_sraiw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(ctx->operands.imm) & 0x1fu;
  uint32_t result = sra32(static_cast<uint32_t>(src1(ctx)), shamt);
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}

int exec_add(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) + src2(ctx));
  return STATE_RUNNING;
}
int exec_sub(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) - src2(ctx));
  return STATE_RUNNING;
}
int exec_sll(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) << shamt);
  return STATE_RUNNING;
}
int exec_slt(IdealContext* ctx) {
  uint64_t result =
      static_cast<int64_t>(src1(ctx)) < static_cast<int64_t>(src2(ctx)) ? 1 : 0;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, result);
  return STATE_RUNNING;
}
int exec_sltu(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) < src2(ctx) ? 1 : 0);
  return STATE_RUNNING;
}
int exec_xor(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) ^ src2(ctx));
  return STATE_RUNNING;
}
int exec_srl(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) >> shamt);
  return STATE_RUNNING;
}
int exec_sra(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x3fu;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sra64(src1(ctx), shamt));
  return STATE_RUNNING;
}
int exec_or(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) | src2(ctx));
  return STATE_RUNNING;
}
int exec_and(IdealContext* ctx) {
  ideal_gpr_write(ctx->hart, ctx->operands.rd, src1(ctx) & src2(ctx));
  return STATE_RUNNING;
}
int exec_addw(IdealContext* ctx) {
  uint32_t result =
      static_cast<uint32_t>(src1(ctx)) + static_cast<uint32_t>(src2(ctx));
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_subw(IdealContext* ctx) {
  uint32_t result =
      static_cast<uint32_t>(src1(ctx)) - static_cast<uint32_t>(src2(ctx));
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_sllw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x1fu;
  uint32_t result = static_cast<uint32_t>(src1(ctx)) << shamt;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_srlw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x1fu;
  uint32_t result = static_cast<uint32_t>(src1(ctx)) >> shamt;
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}
int exec_sraw(IdealContext* ctx) {
  unsigned shamt = static_cast<unsigned>(src2(ctx)) & 0x1fu;
  uint32_t result = sra32(static_cast<uint32_t>(src1(ctx)), shamt);
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext32(result));
  return STATE_RUNNING;
}

int exec_fence(IdealContext*) { return STATE_RUNNING; }
int exec_fence_tso(IdealContext*) { return STATE_RUNNING; }
int exec_pause(IdealContext*) { return STATE_RUNNING; }

int exec_ecall(IdealContext* ctx) {
  uint64_t cause = kExcEcallM;
  if (ctx->hart->priv == 0) {
    cause = kExcEcallU;
  } else if (ctx->hart->priv == 1) {
    cause = kExcEcallS;
  } else if (ctx->hart->priv != 3) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
    return STATE_RUNNING;
  }
  ideal_raise(ctx, cause, 0);
  return STATE_RUNNING;
}

int exec_ebreak(IdealContext* ctx) {
  ideal_raise(ctx, kExcBreakpoint, ctx->hart->pc);
  return STATE_RUNNING;
}

int exec_mret(IdealContext* ctx) {
  IdealHart* hart = ctx->hart;
  if (hart->priv != 3) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
    return STATE_RUNNING;
  }
  uint64_t status = hart->mstatus;
  unsigned previous = static_cast<unsigned>((status >> 11) & 3u);
  bool pie = (status & kMstatusMpie) != 0;
  if (pie) {
    status |= kMstatusMie;
  } else {
    status &= ~kMstatusMie;
  }
  status |= kMstatusMpie;
  status &= ~kMstatusMpp;
  if (previous != 3) {
    status &= ~kMstatusMprv;
  }
  hart->mstatus = status;
  hart->priv = static_cast<unsigned char>(previous);
  ctx->npc = hart->mepc;
  return STATE_RUNNING;
}

int exec_csrrw(IdealContext* ctx) { return ideal_csr(ctx, 0, false); }
int exec_csrrs(IdealContext* ctx) { return ideal_csr(ctx, 1, false); }
int exec_csrrc(IdealContext* ctx) { return ideal_csr(ctx, 2, false); }
int exec_csrrwi(IdealContext* ctx) { return ideal_csr(ctx, 0, true); }
int exec_csrrsi(IdealContext* ctx) { return ideal_csr(ctx, 1, true); }
int exec_csrrci(IdealContext* ctx) { return ideal_csr(ctx, 2, true); }
