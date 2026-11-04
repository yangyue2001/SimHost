#include "service/ideal/exec.h"

#include "service/service.h"

namespace {

uint64_t sext_len(uint64_t value, unsigned size) {
  if (size == 4) {
    return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value)));
  }
  return value;
}

uint64_t mask_len(unsigned size) {
  return size == 8 ? ~0ull : (1ull << (size * 8)) - 1ull;
}

void reserve(IdealHart* hart, uint64_t addr, unsigned size) {
  hart->lr_valid = true;
  hart->lr_addr = addr;
  hart->lr_len = static_cast<uint8_t>(size);
}

bool reserved(const IdealHart* hart, uint64_t addr, unsigned size) {
  return hart->lr_valid && hart->lr_addr == addr && hart->lr_len == size;
}

int amo(IdealContext* ctx, unsigned size, int op) {
  uint64_t addr = ideal_gpr_read(ctx->hart, ctx->operands.rs1);
  uint64_t src = ideal_gpr_read(ctx->hart, ctx->operands.rs2) & mask_len(size);
  ctx->hart->lr_valid = false;
  uint64_t loaded = 0;
  if (!ideal_load_data(ctx, addr, size, &loaded)) {
    return STATE_RUNNING;
  }
  loaded &= mask_len(size);
  int64_t signed_old = static_cast<int64_t>(sext_len(loaded, size));
  int64_t signed_src = static_cast<int64_t>(sext_len(src, size));
  uint64_t result = 0;
  switch (op) {
    case 0:
      result = loaded + src;
      break;
    case 1:
      result = src;
      break;
    case 4:
      result = loaded ^ src;
      break;
    case 8:
      result = loaded | src;
      break;
    case 12:
      result = loaded & src;
      break;
    case 16:
      result = signed_old < signed_src ? loaded : src;
      break;
    case 20:
      result = signed_old > signed_src ? loaded : src;
      break;
    case 24:
      result = loaded < src ? loaded : src;
      break;
    case 28:
      result = loaded > src ? loaded : src;
      break;
    default:
      ideal_raise(ctx, kExcIllegal, ctx->inst);
      return STATE_RUNNING;
  }
  if (!ideal_store_data(ctx, addr, size, result & mask_len(size))) {
    return STATE_RUNNING;
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext_len(loaded, size));
  return STATE_RUNNING;
}

int lr(IdealContext* ctx, unsigned size) {
  uint64_t addr = ideal_gpr_read(ctx->hart, ctx->operands.rs1);
  uint64_t loaded = 0;
  if (!ideal_load_data(ctx, addr, size, &loaded)) {
    return STATE_RUNNING;
  }
  reserve(ctx->hart, addr, size);
  ideal_gpr_write(ctx->hart, ctx->operands.rd, sext_len(loaded, size));
  return STATE_RUNNING;
}

int sc(IdealContext* ctx, unsigned size) {
  uint64_t addr = ideal_gpr_read(ctx->hart, ctx->operands.rs1);
  bool hit = reserved(ctx->hart, addr, size);
  ctx->hart->lr_valid = false;
  if (!hit) {
    uint64_t ignored = 0;
    if (!ideal_load_data(ctx, addr, size, &ignored)) {
      return STATE_RUNNING;
    }
    ideal_gpr_write(ctx->hart, ctx->operands.rd, 1);
    return STATE_RUNNING;
  }
  if (!ideal_store_data(ctx, addr, size,
                        ideal_gpr_read(ctx->hart, ctx->operands.rs2))) {
    return STATE_RUNNING;
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, 0);
  return STATE_RUNNING;
}

}  // namespace

int exec_lr_w(IdealContext* ctx) { return lr(ctx, 4); }
int exec_lr_d(IdealContext* ctx) { return lr(ctx, 8); }
int exec_sc_w(IdealContext* ctx) { return sc(ctx, 4); }
int exec_sc_d(IdealContext* ctx) { return sc(ctx, 8); }
int exec_amoadd_w(IdealContext* ctx) { return amo(ctx, 4, 0); }
int exec_amoadd_d(IdealContext* ctx) { return amo(ctx, 8, 0); }
int exec_amoswap_w(IdealContext* ctx) { return amo(ctx, 4, 1); }
int exec_amoswap_d(IdealContext* ctx) { return amo(ctx, 8, 1); }
int exec_amoxor_w(IdealContext* ctx) { return amo(ctx, 4, 4); }
int exec_amoxor_d(IdealContext* ctx) { return amo(ctx, 8, 4); }
int exec_amoor_w(IdealContext* ctx) { return amo(ctx, 4, 8); }
int exec_amoor_d(IdealContext* ctx) { return amo(ctx, 8, 8); }
int exec_amoand_w(IdealContext* ctx) { return amo(ctx, 4, 12); }
int exec_amoand_d(IdealContext* ctx) { return amo(ctx, 8, 12); }
int exec_amomin_w(IdealContext* ctx) { return amo(ctx, 4, 16); }
int exec_amomin_d(IdealContext* ctx) { return amo(ctx, 8, 16); }
int exec_amomax_w(IdealContext* ctx) { return amo(ctx, 4, 20); }
int exec_amomax_d(IdealContext* ctx) { return amo(ctx, 8, 20); }
int exec_amominu_w(IdealContext* ctx) { return amo(ctx, 4, 24); }
int exec_amominu_d(IdealContext* ctx) { return amo(ctx, 8, 24); }
int exec_amomaxu_w(IdealContext* ctx) { return amo(ctx, 4, 28); }
int exec_amomaxu_d(IdealContext* ctx) { return amo(ctx, 8, 28); }
