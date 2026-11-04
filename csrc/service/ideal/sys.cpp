#include "service/ideal/exec.h"

#include "service/service.h"

int exec_wfi(IdealContext* ctx) {
  if (ctx->hart->priv == 0 ||
      (ctx->hart->priv != 3 && (ctx->hart->mstatus & kMstatusTw) != 0)) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
  }
  return STATE_RUNNING;
}

int exec_sret(IdealContext* ctx) {
  IdealHart* hart = ctx->hart;
  if (hart->priv < 1 ||
      (hart->priv == 1 && (hart->mstatus & kMstatusTsr) != 0)) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
    return STATE_RUNNING;
  }
  uint64_t status = hart->mstatus;
  unsigned previous = (status & kMstatusSpp) != 0 ? 1 : 0;
  if ((status & kMstatusSpie) != 0) {
    status |= kMstatusSie;
  } else {
    status &= ~kMstatusSie;
  }
  status |= kMstatusSpie;
  status &= ~(kMstatusSpp | kMstatusMprv);
  hart->mstatus = status;
  hart->priv = static_cast<unsigned char>(previous);
  ctx->npc = hart->sepc;
  return STATE_RUNNING;
}

int exec_sfence_vma(IdealContext* ctx) {
  if (ctx->hart->priv < 1 ||
      (ctx->hart->priv == 1 && (ctx->hart->mstatus & kMstatusTvm) != 0)) {
    ideal_raise(ctx, kExcIllegal, ctx->inst);
  }
  return STATE_RUNNING;
}

int exec_fence_i(IdealContext*) { return STATE_RUNNING; }
