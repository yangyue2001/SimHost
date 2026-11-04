#include "service/ideal/exec.h"

#include "service/mmu/mmu.h"

#include <cstdint>
#include <cstdio>

namespace {

constexpr uint64_t kRamBase = 0x80000000ull;
constexpr uint64_t kUartBase = 0x10000000ull;
constexpr uint64_t kUartBytes = 0x100ull;
constexpr uint64_t kClintBase = 0x02000000ull;
constexpr uint64_t kClintBytes = 0x10000ull;
constexpr uint64_t kPlicBase = 0x0c000000ull;
constexpr uint64_t kPlicBytes = 0x4000000ull;

uint8_t uart_lcr = 0;
uint8_t uart_ier = 0;
uint32_t plic_priority[32];
uint32_t plic_enable[2];
uint32_t plic_threshold[2];
uint32_t plic_levels = 0;
uint32_t plic_pending = 0;
uint32_t plic_busy = 0;

void plic_refresh_uart() {
  constexpr uint32_t kUart = 1u << 10;
  if ((uart_ier & 0x2u) != 0) {
    plic_levels |= kUart;
    if ((plic_busy & kUart) == 0) {
      plic_pending |= kUart;
      plic_busy |= kUart;
    }
  } else {
    plic_levels &= ~kUart;
  }
}

unsigned plic_best(unsigned context, bool ignore_threshold) {
  uint32_t candidates = plic_pending & plic_enable[context];
  unsigned best = 0;
  for (unsigned id = 1; id < 32; id++) {
    if ((candidates & (1u << id)) == 0) {
      continue;
    }
    if (plic_priority[id] > plic_priority[best]) {
      best = id;
    }
  }
  if (best == 0) {
    return 0;
  }
  if (!ignore_threshold && plic_priority[best] <= plic_threshold[context]) {
    return 0;
  }
  return best;
}

bool plic_word(uint64_t offset, unsigned size, bool store, uint64_t* data) {
  if (size != 4 || (offset & 3ull) != 0) {
    return false;
  }
  if (offset < 32u * 4u) {
    unsigned id = static_cast<unsigned>(offset / 4u);
    if (!store) {
      *data = plic_priority[id];
      return true;
    }
    if (id != 0) {
      plic_priority[id] = static_cast<uint32_t>(*data) & 7u;
    }
    return true;
  }
  if (offset == 0x1000) {
    if (!store) {
      *data = plic_pending;
    }
    return true;
  }
  for (unsigned context = 0; context < 2; context++) {
    if (offset == 0x2000ull + context * 0x80ull) {
      if (!store) {
        *data = plic_enable[context];
      } else {
        plic_enable[context] = static_cast<uint32_t>(*data) & ~1u;
      }
      return true;
    }
    if (offset == 0x200000ull + context * 0x1000ull) {
      if (!store) {
        *data = plic_threshold[context];
      } else {
        plic_threshold[context] = static_cast<uint32_t>(*data) & 7u;
      }
      return true;
    }
    if (offset == 0x200004ull + context * 0x1000ull) {
      if (!store) {
        unsigned id = plic_best(context, true);
        if (id != 0) {
          plic_pending &= ~(1u << id);
        }
        *data = id;
        return true;
      }
      unsigned id = static_cast<unsigned>(*data);
      if (id > 0 && id < 32) {
        uint32_t bit = 1u << id;
        if ((plic_enable[context] & bit) != 0) {
          plic_busy &= ~bit;
          if ((plic_levels & bit) != 0) {
            plic_pending |= bit;
            plic_busy |= bit;
          }
        }
      }
      return true;
    }
  }
  return false;
}

extern "C" uint64_t memory_read(uint64_t offset);
extern "C" void memory_write(uint64_t offset, uint64_t data,
                             unsigned char strobe);
extern "C" unsigned char memory_covers(uint64_t offset, uint64_t len);

unsigned effective_priv(const IdealHart* hart, bool data) {
  if (data && (hart->mstatus & kMstatusMprv) != 0) {
    return static_cast<unsigned>((hart->mstatus >> 11) & 3u);
  }
  return hart->priv;
}

void fill_req(const IdealHart* hart, uint64_t addr, unsigned size, bool store,
              bool data, MmuReq* req) {
  uint64_t cfg0 = 0;
  uint64_t cfg2 = 0;
  for (unsigned i = 0; i < 8; i++) {
    cfg0 |= static_cast<uint64_t>(hart->pmpcfg[i]) << (8u * i);
    cfg2 |= static_cast<uint64_t>(hart->pmpcfg[8u + i]) << (8u * i);
  }
  req->priv = static_cast<unsigned char>(effective_priv(hart, data));
  req->va_hi = static_cast<unsigned>((addr >> 57) & 0x7fu);
  req->vpn4 = static_cast<unsigned>((addr >> 48) & 0x1ffu);
  req->vpn3 = static_cast<unsigned>((addr >> 39) & 0x1ffu);
  req->vpn2 = static_cast<unsigned>((addr >> 30) & 0x1ffu);
  req->vpn1 = static_cast<unsigned>((addr >> 21) & 0x1ffu);
  req->vpn0 = static_cast<unsigned>((addr >> 12) & 0x1ffu);
  req->page_off = static_cast<unsigned>(addr & 0xfffu);
  req->len = size;
  req->satp = hart->satp;
  req->sum = (hart->mstatus & kMstatusSum) != 0 ? 1 : 0;
  req->mxr = (hart->mstatus & kMstatusMxr) != 0 ? 1 : 0;
  req->pmpcfg0 = cfg0;
  req->pmpcfg2 = cfg2;
  for (unsigned i = 0; i < 16; i++) {
    req->pmpaddr[i] = hart->pmpaddr[i];
  }
  if (!data) {
    req->kind = kMmuFetch;
  } else if (store) {
    req->kind = kMmuStore;
  } else {
    req->kind = kMmuLoad;
  }
}

bool translate(const IdealHart* hart, uint64_t addr, unsigned size, bool store,
               bool data, uint64_t* pa, bool* page_fault, bool* access_fault) {
  *page_fault = false;
  *access_fault = false;
  *pa = 0;
  MmuReq req{};
  fill_req(hart, addr, size, store, data, &req);
  MmuResp resp{};
  if (!mmu_translate(req, &resp)) {
    *access_fault = true;
    return false;
  }
  if (resp.page_fault != 0) {
    *page_fault = true;
    return false;
  }
  if (resp.access_fault != 0) {
    *access_fault = true;
    return false;
  }
  *pa = (resp.ppn << 12) | (addr & 0xfffu);
  return true;
}

bool first_byte_in(uint64_t addr, unsigned size, uint64_t base, uint64_t bytes) {
  if (size == 0 || addr < base) {
    return false;
  }
  return addr - base < bytes;
}

bool uart_access(IdealContext* ctx, uint64_t addr, unsigned size, bool store,
                 uint64_t* data) {
  if (addr < kUartBase || addr >= kUartBase + kUartBytes) {
    return false;
  }
  ctx->mmio.valid = 1;
  ctx->mmio.store = store ? 1 : 0;
  ctx->mmio.size = static_cast<unsigned char>(size);
  ctx->mmio.addr = addr;
  ctx->mmio.data = store ? *data : 0;
  if (size != 1 || (addr & (static_cast<uint64_t>(size) - 1ull)) != 0) {
    ideal_raise(ctx, store ? kExcStoreMisaligned : kExcLoadMisaligned, addr);
    return true;
  }
  uint64_t offset = addr - kUartBase;
  if (offset >= 8) {
    ideal_raise(ctx, store ? kExcStoreAccess : kExcLoadAccess, addr);
    return true;
  }
  if (store) {
    uint8_t byte = static_cast<uint8_t>(*data);
    ctx->mmio.data = byte;
    if (offset == 0 && (uart_lcr & 0x80u) == 0) {
      std::fputc(byte, stdout);
      std::fflush(stdout);
    } else if (offset == 1) {
      uart_ier = static_cast<uint8_t>(byte & 0x0fu);
      plic_refresh_uart();
    } else if (offset == 3) {
      uart_lcr = byte;
    }
    return true;
  }
  if (offset == 1) {
    *data = uart_ier;
  } else if (offset == 2) {
    *data = (uart_ier & 0x2u) != 0 ? 0x02 : 0x01;
  } else if (offset == 3) {
    *data = uart_lcr;
  } else if (offset == 5) {
    *data = 0x60;
  } else {
    *data = 0;
  }
  ctx->mmio.data = *data;
  return true;
}

bool zero_device(IdealContext* ctx, uint64_t addr, unsigned size, bool store,
                 uint64_t* data) {
  bool clint = first_byte_in(addr, size, kClintBase, kClintBytes);
  bool plic = first_byte_in(addr, size, kPlicBase, kPlicBytes);
  if (!clint && !plic) {
    return false;
  }
  uint64_t base = clint ? kClintBase : kPlicBase;
  uint64_t bytes = clint ? kClintBytes : kPlicBytes;
  uint64_t mask = size >= 8 ? ~0ull : (1ull << (size * 8)) - 1ull;
  ctx->mmio.valid = 1;
  ctx->mmio.store = store ? 1 : 0;
  ctx->mmio.size = static_cast<unsigned char>(size);
  ctx->mmio.addr = addr;
  ctx->mmio.data = store ? (*data & mask) : 0;
  if ((addr & (static_cast<uint64_t>(size) - 1ull)) != 0) {
    ideal_raise(ctx, store ? kExcStoreMisaligned : kExcLoadMisaligned, addr);
    return true;
  }
  if (addr - base > bytes - size) {
    ideal_raise(ctx, store ? kExcStoreAccess : kExcLoadAccess, addr);
    return true;
  }
  if (plic && plic_word(addr - base, size, store, data)) {
    if (!store) {
      ctx->mmio.data = *data;
    }
    return true;
  }
  if (!store) {
    *data = 0;
  }
  return true;
}

bool ram_access(uint64_t addr, unsigned size, bool store, uint64_t* data) {
  if (addr < kRamBase) {
    return false;
  }
  uint64_t offset = addr - kRamBase;
  if (memory_covers(offset, size) == 0) {
    return false;
  }
  uint64_t aligned = offset & ~7ull;
  unsigned shift = static_cast<unsigned>(addr & 7u) * 8u;
  if (!store) {
    uint64_t word = memory_read(aligned);
    uint64_t mask = size == 8 ? ~0ull : (1ull << (size * 8)) - 1ull;
    *data = (word >> shift) & mask;
    return true;
  }
  unsigned char strobe =
      static_cast<unsigned char>(((1u << size) - 1u) << (addr & 7u));
  memory_write(aligned, *data << shift, strobe);
  return true;
}

bool access(IdealContext* ctx, uint64_t addr, unsigned size, bool store,
            uint64_t* data) {
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    ideal_raise(ctx, store ? kExcStoreAccess : kExcLoadAccess, addr);
    return false;
  }
  if ((addr & (static_cast<uint64_t>(size) - 1ull)) != 0) {
    ideal_raise(ctx, store ? kExcStoreMisaligned : kExcLoadMisaligned, addr);
    return false;
  }
  uint64_t pa = 0;
  bool page = false;
  bool fault = false;
  if (!translate(ctx->hart, addr, size, store, true, &pa, &page, &fault)) {
    if (page) {
      ideal_raise(ctx, store ? kExcStorePage : kExcLoadPage, addr);
    } else {
      ideal_raise(ctx, store ? kExcStoreAccess : kExcLoadAccess, addr);
    }
    return false;
  }
  if (uart_access(ctx, pa, size, store, data) ||
      zero_device(ctx, pa, size, store, data)) {
    if (!ctx->trapped && store) {
      ctx->hart->lr_valid = false;
    }
    return !ctx->trapped;
  }
  if (!ram_access(pa, size, store, data)) {
    ideal_raise(ctx, store ? kExcStoreAccess : kExcLoadAccess, addr);
    return false;
  }
  if (store) {
    ctx->hart->lr_valid = false;
  }
  return true;
}

}  // namespace

void ideal_uart_reset() {
  uart_lcr = 0;
  uart_ier = 0;
  plic_levels = 0;
  plic_pending = 0;
  plic_busy = 0;
  for (unsigned i = 0; i < 32; i++) {
    plic_priority[i] = 0;
  }
  plic_enable[0] = 0;
  plic_enable[1] = 0;
  plic_threshold[0] = 0;
  plic_threshold[1] = 0;
}

uint64_t ideal_device_mip() {
  uint64_t mip = 0;
  if (plic_best(0, false) != 0) {
    mip |= 1ull << 11;
  }
  if (plic_best(1, false) != 0) {
    mip |= 1ull << 9;
  }
  return mip;
}

bool ideal_load_data(IdealContext* ctx, uint64_t addr, unsigned size,
                     uint64_t* value) {
  return access(ctx, addr, size, false, value);
}

bool ideal_store_data(IdealContext* ctx, uint64_t addr, unsigned size,
                      uint64_t value) {
  return access(ctx, addr, size, true, &value);
}

bool ideal_translate_fetch(const IdealHart* hart, uint64_t pc, unsigned len,
                           uint64_t* pa, bool* page_fault, bool* access_fault) {
  return translate(hart, pc, len, false, false, pa, page_fault, access_fault);
}
