#include "service/ideal/exec.h"

#include "service/service.h"

#include "softfloat.h"

namespace {

constexpr unsigned kFp32Bits = 32;
constexpr uint64_t kFp32Sign = 1ull << 31;
constexpr uint64_t kFp64Sign = 1ull << 63;
constexpr uint64_t kFp32Box = ~0ull << 32;

bool fp_on(const IdealHart* hart) { return (hart->mstatus & kMstatusFs) != 0; }

void fp_dirty(IdealHart* hart) {
  hart->mstatus = (hart->mstatus & ~kMstatusFs) | kMstatusFsDirty;
}

bool fp_round(IdealContext* ctx) {
  unsigned rm = ctx->operands.rm;
  if (rm == 7) {
    rm = static_cast<unsigned>((ctx->hart->fcsr >> 5) & 7u);
  }
  if (rm > 4) {
    return false;
  }
  softfloat_roundingMode = static_cast<uint_fast8_t>(rm);
  softfloat_detectTininess = softfloat_tininess_afterRounding;
  return true;
}

void fp_begin() { softfloat_exceptionFlags = 0; }

void fp_finish(IdealContext* ctx, bool writes_fpr) {
  uint_fast8_t flags =
      static_cast<uint_fast8_t>(softfloat_exceptionFlags & 0x1fu);
  if (flags != 0) {
    ctx->hart->fcsr |= flags;
    fp_dirty(ctx->hart);
  }
  if (writes_fpr) {
    fp_dirty(ctx->hart);
  }
  softfloat_exceptionFlags = 0;
}

bool fp_ok(IdealContext* ctx, bool rounding) {
  if (!fp_on(ctx->hart) || (rounding && !fp_round(ctx))) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
    return false;
  }
  return true;
}

uint64_t fp32_read(const IdealHart* hart, uint8_t rs) {
  uint64_t value = hart->fpr[rs];
  if ((value >> kFp32Bits) != 0xffffffffu) {
    return 0x7fc00000ull;
  }
  return static_cast<uint32_t>(value);
}

void fp32_write(IdealHart* hart, uint8_t rd, uint64_t value) {
  hart->fpr[rd] = kFp32Box | static_cast<uint32_t>(value);
}

uint64_t fp64_read(const IdealHart* hart, uint8_t rs) { return hart->fpr[rs]; }

void fp64_write(IdealHart* hart, uint8_t rd, uint64_t value) {
  hart->fpr[rd] = value;
}

uint64_t fp_sign(uint64_t bits, unsigned mant, unsigned exp) {
  return 1ull << (mant + exp);
}

uint64_t fp_exp_mask(unsigned mant, unsigned exp) {
  return ((1ull << exp) - 1ull) << mant;
}

uint64_t fp_mant_mask(unsigned mant) { return (1ull << mant) - 1ull; }

bool fp_nan(uint64_t bits, unsigned mant, unsigned exp) {
  uint64_t exp_mask = fp_exp_mask(mant, exp);
  return (bits & exp_mask) == exp_mask && (bits & fp_mant_mask(mant)) != 0;
}

bool fp_snan(uint64_t bits, unsigned mant, unsigned exp) {
  return fp_nan(bits, mant, exp) && (bits & (1ull << (mant - 1))) == 0;
}

uint64_t fp_canonical(unsigned mant, unsigned exp) {
  return fp_exp_mask(mant, exp) | (1ull << (mant - 1));
}

uint64_t fp_class(uint64_t bits, unsigned mant, unsigned exp) {
  uint64_t exp_max = (1ull << exp) - 1ull;
  bool sign = (bits & fp_sign(bits, mant, exp)) != 0;
  uint64_t exponent = (bits >> mant) & exp_max;
  uint64_t fraction = bits & fp_mant_mask(mant);
  int cls = 0;
  if (exponent == exp_max) {
    if (fraction != 0) {
      cls = fp_snan(bits, mant, exp) ? 8 : 9;
    } else {
      cls = sign ? 0 : 7;
    }
  } else if (exponent == 0) {
    if (fraction != 0) {
      cls = sign ? 2 : 5;
    } else {
      cls = sign ? 3 : 4;
    }
  } else {
    cls = sign ? 1 : 6;
  }
  return 1ull << cls;
}

uint64_t fp_minmax(uint64_t lhs, uint64_t rhs, bool max, unsigned mant,
                   unsigned exp, bool (*less)(uint64_t, uint64_t)) {
  uint64_t sign = fp_sign(lhs, mant, exp);
  if (fp_snan(lhs, mant, exp) || fp_snan(rhs, mant, exp)) {
    softfloat_exceptionFlags |= softfloat_flag_invalid;
  }
  bool lhs_nan = fp_nan(lhs, mant, exp);
  bool rhs_nan = fp_nan(rhs, mant, exp);
  if (lhs_nan && rhs_nan) {
    return fp_canonical(mant, exp);
  }
  if (lhs_nan) {
    return rhs;
  }
  if (rhs_nan) {
    return lhs;
  }
  if ((lhs & ~sign) == 0 && (rhs & ~sign) == 0) {
    return max ? (lhs & rhs) : (lhs | rhs);
  }
  bool lhs_less = less(lhs, rhs);
  return max ? (lhs_less ? rhs : lhs) : (lhs_less ? lhs : rhs);
}

uint64_t inject(uint64_t lhs, uint64_t rhs, uint64_t sign, bool negate,
                bool xor_sign) {
  uint64_t selected = xor_sign ? (lhs ^ rhs) : (negate ? ~rhs : rhs);
  return (lhs & ~sign) | (selected & sign);
}

bool f32_less(uint64_t lhs, uint64_t rhs) {
  return f32_lt_quiet(float32_t{static_cast<uint32_t>(lhs)},
                      float32_t{static_cast<uint32_t>(rhs)});
}

bool f64_less(uint64_t lhs, uint64_t rhs) {
  return f64_lt_quiet(float64_t{lhs}, float64_t{rhs});
}

int load_fp(IdealContext* ctx, unsigned size, bool single) {
  if (!fp_ok(ctx, false)) {
    return STATE_RUNNING;
  }
  uint64_t addr = ideal_gpr_read(ctx->hart, ctx->operands.rs1) + ctx->operands.imm;
  uint64_t raw = 0;
  if (!ideal_load_data(ctx, addr, size, &raw)) {
    return STATE_RUNNING;
  }
  if (single) {
    fp32_write(ctx->hart, ctx->operands.rd, raw);
  } else {
    fp64_write(ctx->hart, ctx->operands.rd, raw);
  }
  fp_dirty(ctx->hart);
  return STATE_RUNNING;
}

int store_fp(IdealContext* ctx, unsigned size, bool single) {
  if (!fp_ok(ctx, false)) {
    return STATE_RUNNING;
  }
  uint64_t addr = ideal_gpr_read(ctx->hart, ctx->operands.rs1) + ctx->operands.imm;
  uint64_t raw = ctx->hart->fpr[ctx->operands.rs2];
  if (single) {
    raw = static_cast<uint32_t>(raw);
  }
  ideal_store_data(ctx, addr, size, raw);
  return STATE_RUNNING;
}

template <typename Op>
int binary_fp(IdealContext* ctx, bool single, Op op) {
  if (!fp_ok(ctx, true)) {
    return STATE_RUNNING;
  }
  fp_begin();
  uint64_t lhs = single ? fp32_read(ctx->hart, ctx->operands.rs1)
                        : fp64_read(ctx->hart, ctx->operands.rs1);
  uint64_t rhs = single ? fp32_read(ctx->hart, ctx->operands.rs2)
                        : fp64_read(ctx->hart, ctx->operands.rs2);
  uint64_t result = op(lhs, rhs);
  if (single) {
    fp32_write(ctx->hart, ctx->operands.rd, result);
  } else {
    fp64_write(ctx->hart, ctx->operands.rd, result);
  }
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

int fma(IdealContext* ctx, bool single, bool neg_product, bool neg_addend) {
  if (!fp_ok(ctx, true)) {
    return STATE_RUNNING;
  }
  fp_begin();
  uint64_t sign = single ? kFp32Sign : kFp64Sign;
  uint64_t lhs = single ? fp32_read(ctx->hart, ctx->operands.rs1)
                        : fp64_read(ctx->hart, ctx->operands.rs1);
  uint64_t rhs = single ? fp32_read(ctx->hart, ctx->operands.rs2)
                        : fp64_read(ctx->hart, ctx->operands.rs2);
  uint64_t add = single ? fp32_read(ctx->hart, ctx->operands.rs3)
                        : fp64_read(ctx->hart, ctx->operands.rs3);
  if (neg_product) {
    lhs ^= sign;
  }
  if (neg_addend) {
    add ^= sign;
  }
  uint64_t result = 0;
  if (single) {
    result = f32_mulAdd(float32_t{static_cast<uint32_t>(lhs)},
                        float32_t{static_cast<uint32_t>(rhs)},
                        float32_t{static_cast<uint32_t>(add)})
                 .v;
    fp32_write(ctx->hart, ctx->operands.rd, result);
  } else {
    result = f64_mulAdd(float64_t{lhs}, float64_t{rhs}, float64_t{add}).v;
    fp64_write(ctx->hart, ctx->operands.rd, result);
  }
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

template <typename Op>
int to_int(IdealContext* ctx, bool word, Op op) {
  if (!fp_ok(ctx, true)) {
    return STATE_RUNNING;
  }
  fp_begin();
  uint64_t result = op();
  if (word) {
    result = static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(result)));
  }
  ideal_gpr_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, false);
  return STATE_RUNNING;
}

template <typename Op>
int from_int(IdealContext* ctx, bool single, Op op) {
  if (!fp_ok(ctx, true)) {
    return STATE_RUNNING;
  }
  fp_begin();
  uint64_t value = ideal_gpr_read(ctx->hart, ctx->operands.rs1);
  uint64_t result = op(value);
  if (single) {
    fp32_write(ctx->hart, ctx->operands.rd, result);
  } else {
    fp64_write(ctx->hart, ctx->operands.rd, result);
  }
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

int cmp(IdealContext* ctx, bool single, bool (*op)(uint64_t, uint64_t)) {
  if (!fp_ok(ctx, false)) {
    return STATE_RUNNING;
  }
  fp_begin();
  uint64_t lhs = single ? fp32_read(ctx->hart, ctx->operands.rs1)
                        : fp64_read(ctx->hart, ctx->operands.rs1);
  uint64_t rhs = single ? fp32_read(ctx->hart, ctx->operands.rs2)
                        : fp64_read(ctx->hart, ctx->operands.rs2);
  ideal_gpr_write(ctx->hart, ctx->operands.rd, op(lhs, rhs) ? 1 : 0);
  fp_finish(ctx, false);
  return STATE_RUNNING;
}

int sgnj(IdealContext* ctx, bool single, bool negate, bool xor_sign) {
  if (!fp_ok(ctx, false)) {
    return STATE_RUNNING;
  }
  uint64_t sign = single ? kFp32Sign : kFp64Sign;
  uint64_t lhs = single ? fp32_read(ctx->hart, ctx->operands.rs1)
                        : fp64_read(ctx->hart, ctx->operands.rs1);
  uint64_t rhs = single ? fp32_read(ctx->hart, ctx->operands.rs2)
                        : fp64_read(ctx->hart, ctx->operands.rs2);
  uint64_t result = inject(lhs, rhs, sign, negate, xor_sign);
  if (single) {
    fp32_write(ctx->hart, ctx->operands.rd, result);
  } else {
    fp64_write(ctx->hart, ctx->operands.rd, result);
  }
  fp_dirty(ctx->hart);
  return STATE_RUNNING;
}

bool eq32(uint64_t lhs, uint64_t rhs) {
  return f32_eq(float32_t{static_cast<uint32_t>(lhs)},
                float32_t{static_cast<uint32_t>(rhs)});
}
bool lt32(uint64_t lhs, uint64_t rhs) {
  return f32_lt(float32_t{static_cast<uint32_t>(lhs)},
                float32_t{static_cast<uint32_t>(rhs)});
}
bool le32(uint64_t lhs, uint64_t rhs) {
  return f32_le(float32_t{static_cast<uint32_t>(lhs)},
                float32_t{static_cast<uint32_t>(rhs)});
}
bool eq64(uint64_t lhs, uint64_t rhs) {
  return f64_eq(float64_t{lhs}, float64_t{rhs});
}
bool lt64(uint64_t lhs, uint64_t rhs) {
  return f64_lt(float64_t{lhs}, float64_t{rhs});
}
bool le64(uint64_t lhs, uint64_t rhs) {
  return f64_le(float64_t{lhs}, float64_t{rhs});
}

}  // namespace

int exec_flw(IdealContext* ctx) { return load_fp(ctx, 4, true); }
int exec_fsw(IdealContext* ctx) { return store_fp(ctx, 4, true); }
int exec_fld(IdealContext* ctx) { return load_fp(ctx, 8, false); }
int exec_fsd(IdealContext* ctx) { return store_fp(ctx, 8, false); }

int exec_fmadd_s(IdealContext* ctx) { return fma(ctx, true, false, false); }
int exec_fmsub_s(IdealContext* ctx) { return fma(ctx, true, false, true); }
int exec_fnmsub_s(IdealContext* ctx) { return fma(ctx, true, true, false); }
int exec_fnmadd_s(IdealContext* ctx) { return fma(ctx, true, true, true); }
int exec_fmadd_d(IdealContext* ctx) { return fma(ctx, false, false, false); }
int exec_fmsub_d(IdealContext* ctx) { return fma(ctx, false, false, true); }
int exec_fnmsub_d(IdealContext* ctx) { return fma(ctx, false, true, false); }
int exec_fnmadd_d(IdealContext* ctx) { return fma(ctx, false, true, true); }

int exec_fadd_s(IdealContext* ctx) {
  return binary_fp(ctx, true, [](uint64_t a, uint64_t b) {
    return static_cast<uint64_t>(
        f32_add(float32_t{static_cast<uint32_t>(a)},
                float32_t{static_cast<uint32_t>(b)})
            .v);
  });
}
int exec_fsub_s(IdealContext* ctx) {
  return binary_fp(ctx, true, [](uint64_t a, uint64_t b) {
    return static_cast<uint64_t>(
        f32_sub(float32_t{static_cast<uint32_t>(a)},
                float32_t{static_cast<uint32_t>(b)})
            .v);
  });
}
int exec_fmul_s(IdealContext* ctx) {
  return binary_fp(ctx, true, [](uint64_t a, uint64_t b) {
    return static_cast<uint64_t>(
        f32_mul(float32_t{static_cast<uint32_t>(a)},
                float32_t{static_cast<uint32_t>(b)})
            .v);
  });
}
int exec_fdiv_s(IdealContext* ctx) {
  return binary_fp(ctx, true, [](uint64_t a, uint64_t b) {
    return static_cast<uint64_t>(
        f32_div(float32_t{static_cast<uint32_t>(a)},
                float32_t{static_cast<uint32_t>(b)})
            .v);
  });
}
int exec_fsqrt_s(IdealContext* ctx) {
  if (!fp_ok(ctx, true)) return STATE_RUNNING;
  fp_begin();
  uint64_t result =
      f32_sqrt(float32_t{static_cast<uint32_t>(fp32_read(ctx->hart, ctx->operands.rs1))}).v;
  fp32_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}
int exec_fadd_d(IdealContext* ctx) {
  return binary_fp(ctx, false, [](uint64_t a, uint64_t b) {
    return f64_add(float64_t{a}, float64_t{b}).v;
  });
}
int exec_fsub_d(IdealContext* ctx) {
  return binary_fp(ctx, false, [](uint64_t a, uint64_t b) {
    return f64_sub(float64_t{a}, float64_t{b}).v;
  });
}
int exec_fmul_d(IdealContext* ctx) {
  return binary_fp(ctx, false, [](uint64_t a, uint64_t b) {
    return f64_mul(float64_t{a}, float64_t{b}).v;
  });
}
int exec_fdiv_d(IdealContext* ctx) {
  return binary_fp(ctx, false, [](uint64_t a, uint64_t b) {
    return f64_div(float64_t{a}, float64_t{b}).v;
  });
}
int exec_fsqrt_d(IdealContext* ctx) {
  if (!fp_ok(ctx, true)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = f64_sqrt(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)}).v;
  fp64_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

int exec_fsgnj_s(IdealContext* ctx) { return sgnj(ctx, true, false, false); }
int exec_fsgnjn_s(IdealContext* ctx) { return sgnj(ctx, true, true, false); }
int exec_fsgnjx_s(IdealContext* ctx) { return sgnj(ctx, true, false, true); }
int exec_fsgnj_d(IdealContext* ctx) { return sgnj(ctx, false, false, false); }
int exec_fsgnjn_d(IdealContext* ctx) { return sgnj(ctx, false, true, false); }
int exec_fsgnjx_d(IdealContext* ctx) { return sgnj(ctx, false, false, true); }

int exec_fmin_s(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = fp_minmax(fp32_read(ctx->hart, ctx->operands.rs1),
                              fp32_read(ctx->hart, ctx->operands.rs2), false, 23,
                              8, f32_less);
  fp32_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}
int exec_fmax_s(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = fp_minmax(fp32_read(ctx->hart, ctx->operands.rs1),
                              fp32_read(ctx->hart, ctx->operands.rs2), true, 23, 8,
                              f32_less);
  fp32_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}
int exec_fmin_d(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = fp_minmax(fp64_read(ctx->hart, ctx->operands.rs1),
                              fp64_read(ctx->hart, ctx->operands.rs2), false, 52,
                              11, f64_less);
  fp64_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}
int exec_fmax_d(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = fp_minmax(fp64_read(ctx->hart, ctx->operands.rs1),
                              fp64_read(ctx->hart, ctx->operands.rs2), true, 52,
                              11, f64_less);
  fp64_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

int exec_fcvt_w_s(IdealContext* ctx) {
  return to_int(ctx, true, [&] {
    return static_cast<uint64_t>(static_cast<int32_t>(f32_to_i32(
        float32_t{static_cast<uint32_t>(fp32_read(ctx->hart, ctx->operands.rs1))},
        softfloat_roundingMode, true)));
  });
}
int exec_fcvt_wu_s(IdealContext* ctx) {
  return to_int(ctx, true, [&] {
    return static_cast<uint64_t>(f32_to_ui32(
        float32_t{static_cast<uint32_t>(fp32_read(ctx->hart, ctx->operands.rs1))},
        softfloat_roundingMode, true));
  });
}
int exec_fcvt_l_s(IdealContext* ctx) {
  return to_int(ctx, false, [&] {
    return static_cast<uint64_t>(f32_to_i64(
        float32_t{static_cast<uint32_t>(fp32_read(ctx->hart, ctx->operands.rs1))},
        softfloat_roundingMode, true));
  });
}
int exec_fcvt_lu_s(IdealContext* ctx) {
  return to_int(ctx, false, [&] {
    return static_cast<uint64_t>(f32_to_ui64(
        float32_t{static_cast<uint32_t>(fp32_read(ctx->hart, ctx->operands.rs1))},
        softfloat_roundingMode, true));
  });
}
int exec_fcvt_w_d(IdealContext* ctx) {
  return to_int(ctx, true, [&] {
    return static_cast<uint64_t>(static_cast<int32_t>(
        f64_to_i32(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)},
                   softfloat_roundingMode, true)));
  });
}
int exec_fcvt_wu_d(IdealContext* ctx) {
  return to_int(ctx, true, [&] {
    return static_cast<uint64_t>(
        f64_to_ui32(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)},
                    softfloat_roundingMode, true));
  });
}
int exec_fcvt_l_d(IdealContext* ctx) {
  return to_int(ctx, false, [&] {
    return static_cast<uint64_t>(
        f64_to_i64(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)},
                   softfloat_roundingMode, true));
  });
}
int exec_fcvt_lu_d(IdealContext* ctx) {
  return to_int(ctx, false, [&] {
    return static_cast<uint64_t>(
        f64_to_ui64(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)},
                    softfloat_roundingMode, true));
  });
}

int exec_fcvt_s_w(IdealContext* ctx) {
  return from_int(ctx, true, [](uint64_t value) {
    return static_cast<uint64_t>(i32_to_f32(static_cast<int32_t>(value)).v);
  });
}
int exec_fcvt_s_wu(IdealContext* ctx) {
  return from_int(ctx, true, [](uint64_t value) {
    return static_cast<uint64_t>(ui32_to_f32(static_cast<uint32_t>(value)).v);
  });
}
int exec_fcvt_s_l(IdealContext* ctx) {
  return from_int(ctx, true, [](uint64_t value) {
    return static_cast<uint64_t>(i64_to_f32(static_cast<int64_t>(value)).v);
  });
}
int exec_fcvt_s_lu(IdealContext* ctx) {
  return from_int(ctx, true, [](uint64_t value) {
    return static_cast<uint64_t>(ui64_to_f32(value).v);
  });
}
int exec_fcvt_d_w(IdealContext* ctx) {
  return from_int(ctx, false, [](uint64_t value) {
    return i32_to_f64(static_cast<int32_t>(value)).v;
  });
}
int exec_fcvt_d_wu(IdealContext* ctx) {
  return from_int(ctx, false, [](uint64_t value) {
    return ui32_to_f64(static_cast<uint32_t>(value)).v;
  });
}
int exec_fcvt_d_l(IdealContext* ctx) {
  return from_int(ctx, false, [](uint64_t value) {
    return i64_to_f64(static_cast<int64_t>(value)).v;
  });
}
int exec_fcvt_d_lu(IdealContext* ctx) {
  return from_int(ctx, false, [](uint64_t value) { return ui64_to_f64(value).v; });
}

int exec_fcvt_s_d(IdealContext* ctx) {
  if (!fp_ok(ctx, true)) return STATE_RUNNING;
  fp_begin();
  uint64_t result =
      f64_to_f32(float64_t{fp64_read(ctx->hart, ctx->operands.rs1)}).v;
  fp32_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}
int exec_fcvt_d_s(IdealContext* ctx) {
  if (!fp_ok(ctx, true)) return STATE_RUNNING;
  fp_begin();
  uint64_t result = f32_to_f64(float32_t{static_cast<uint32_t>(
                                   fp32_read(ctx->hart, ctx->operands.rs1))})
                        .v;
  fp64_write(ctx->hart, ctx->operands.rd, result);
  fp_finish(ctx, true);
  return STATE_RUNNING;
}

int exec_feq_s(IdealContext* ctx) { return cmp(ctx, true, eq32); }
int exec_flt_s(IdealContext* ctx) { return cmp(ctx, true, lt32); }
int exec_fle_s(IdealContext* ctx) { return cmp(ctx, true, le32); }
int exec_feq_d(IdealContext* ctx) { return cmp(ctx, false, eq64); }
int exec_flt_d(IdealContext* ctx) { return cmp(ctx, false, lt64); }
int exec_fle_d(IdealContext* ctx) { return cmp(ctx, false, le64); }

int exec_fclass_s(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  ideal_gpr_write(ctx->hart, ctx->operands.rd,
                  fp_class(fp32_read(ctx->hart, ctx->operands.rs1), 23, 8));
  return STATE_RUNNING;
}
int exec_fclass_d(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  ideal_gpr_write(ctx->hart, ctx->operands.rd,
                  fp_class(fp64_read(ctx->hart, ctx->operands.rs1), 52, 11));
  return STATE_RUNNING;
}

int exec_fmv_x_w(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  uint32_t raw = static_cast<uint32_t>(ctx->hart->fpr[ctx->operands.rs1]);
  ideal_gpr_write(ctx->hart, ctx->operands.rd,
                  static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw))));
  return STATE_RUNNING;
}
int exec_fmv_w_x(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp32_write(ctx->hart, ctx->operands.rd,
             ideal_gpr_read(ctx->hart, ctx->operands.rs1));
  fp_dirty(ctx->hart);
  return STATE_RUNNING;
}
int exec_fmv_x_d(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  ideal_gpr_write(ctx->hart, ctx->operands.rd,
                  ctx->hart->fpr[ctx->operands.rs1]);
  return STATE_RUNNING;
}
int exec_fmv_d_x(IdealContext* ctx) {
  if (!fp_ok(ctx, false)) return STATE_RUNNING;
  fp64_write(ctx->hart, ctx->operands.rd,
             ideal_gpr_read(ctx->hart, ctx->operands.rs1));
  fp_dirty(ctx->hart);
  return STATE_RUNNING;
}
