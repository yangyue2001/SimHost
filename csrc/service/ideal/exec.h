#ifndef SIM_SERVICE_IDEAL_EXEC_H
#define SIM_SERVICE_IDEAL_EXEC_H

#include <cstdint>

// Architectural state for one RV64GC hart. CLINT and PLIC windows read as
// zero and writes are dropped. `time` advances once per step. Sstc is the
// timer Linux uses: when menvcfg.STCE is set, STIP tracks time >= stimecmp.
struct IdealHart {
  uint64_t gpr[32];
  uint64_t fpr[32];
  uint64_t pc;
  uint64_t mstatus;
  uint64_t medeleg;
  uint64_t mideleg;
  uint64_t mie;
  uint64_t mip;
  uint64_t mtvec;
  uint64_t mscratch;
  uint64_t mepc;
  uint64_t mcause;
  uint64_t mtval;
  uint64_t mcounteren;
  uint64_t mcountinhibit;
  uint64_t menvcfg;
  uint64_t mcycle;
  uint64_t minstret;
  uint64_t stvec;
  uint64_t scounteren;
  uint64_t senvcfg;
  uint64_t sscratch;
  uint64_t sepc;
  uint64_t scause;
  uint64_t stval;
  uint64_t satp;
  uint64_t stimecmp;
  uint64_t fcsr;
  uint64_t time;
  uint64_t pmpaddr[16];
  uint8_t pmpcfg[16];
  unsigned char priv;
  bool lr_valid;
  uint64_t lr_addr;
  uint8_t lr_len;
};

constexpr uint64_t kExcInstMisaligned = 0;
constexpr uint64_t kExcInstAccess = 1;
constexpr uint64_t kExcIllegal = 2;
constexpr uint64_t kExcBreakpoint = 3;
constexpr uint64_t kExcLoadMisaligned = 4;
constexpr uint64_t kExcLoadAccess = 5;
constexpr uint64_t kExcStoreMisaligned = 6;
constexpr uint64_t kExcStoreAccess = 7;
constexpr uint64_t kExcEcallU = 8;
constexpr uint64_t kExcEcallS = 9;
constexpr uint64_t kExcEcallM = 11;
constexpr uint64_t kExcInstPage = 12;
constexpr uint64_t kExcLoadPage = 13;
constexpr uint64_t kExcStorePage = 15;

constexpr uint64_t kMstatusSie = 1ull << 1;
constexpr uint64_t kMstatusMie = 1ull << 3;
constexpr uint64_t kMstatusSpie = 1ull << 5;
constexpr uint64_t kMstatusMpie = 1ull << 7;
constexpr uint64_t kMstatusSpp = 1ull << 8;
constexpr uint64_t kMstatusMpp = 3ull << 11;
constexpr uint64_t kMstatusFs = 3ull << 13;
constexpr uint64_t kMstatusFsDirty = 3ull << 13;
constexpr uint64_t kMstatusMprv = 1ull << 17;
constexpr uint64_t kMstatusSum = 1ull << 18;
constexpr uint64_t kMstatusMxr = 1ull << 19;
constexpr uint64_t kMstatusTvm = 1ull << 20;
constexpr uint64_t kMstatusTw = 1ull << 21;
constexpr uint64_t kMstatusTsr = 1ull << 22;
constexpr uint64_t kMstatusUxl = 2ull << 32;
constexpr uint64_t kMstatusSxl = 2ull << 34;
constexpr uint64_t kMstatusSd = 1ull << 63;

constexpr uint64_t kMisa =
    (2ull << 62) | (1ull << 0) | (1ull << 2) | (1ull << 3) | (1ull << 5) |
    (1ull << 8) | (1ull << 12) | (1ull << 18) | (1ull << 20);

struct IdealOperands {
  uint8_t rd;
  uint8_t rs1;
  uint8_t rs2;
  uint8_t rs3;
  uint8_t rm;
  uint64_t imm;
};

struct IdealMmio {
  unsigned char valid;
  unsigned char store;
  unsigned char size;
  uint64_t addr;
  uint64_t data;
};

struct IdealContext {
  IdealHart* hart;
  uint64_t npc;
  uint32_t inst;
  unsigned len;
  bool trapped;
  IdealOperands operands;
  IdealMmio mmio;
};

typedef int (*IdealExec)(IdealContext* ctx);

struct IdealDecoded {
  IdealExec exec;
  IdealOperands operands;
  bool illegal;
};

void ideal_decode_init();
bool ideal_decode(uint32_t inst, IdealDecoded* out);
// Low two bits not 11: expand a legal RVC parcel into `widened` and return 2.
// A 32-bit encoding is copied through and returns 4. Reserved parcels return 0.
int ideal_widen(uint32_t inst, uint32_t* widened);

uint64_t ideal_gpr_read(const IdealHart* hart, uint8_t rs);
void ideal_gpr_write(IdealHart* hart, uint8_t rd, uint64_t value);

// Exceptions use medeleg. Interrupts use mideleg. Cause bit 63 selects
// which one. A delegated trap stays in S; anything else goes to M.
void ideal_raise(IdealContext* ctx, uint64_t cause, uint64_t tval);
void ideal_fix_sd(IdealHart* hart);
void ideal_count(IdealHart* hart, bool retired);
void ideal_update_timer(IdealHart* hart);
// Returns an interrupt cause with bit 63 set, or 0 when none is enabled.
uint64_t ideal_pending_interrupt(const IdealHart* hart);

// CSR ops. op 0 writes, 1 sets, 2 clears. imm selects the 5-bit zimm.
int ideal_csr(IdealContext* ctx, int op, bool imm);

// Data access. The effective address is translated by IdealMmu, then the
// physical address hits UART, the zero CLINT/PLIC windows, or RAM.
// False means the access trapped and the destination must not be written.
bool ideal_load_data(IdealContext* ctx, uint64_t addr, unsigned size,
                     uint64_t* value);
bool ideal_store_data(IdealContext* ctx, uint64_t addr, unsigned size,
                      uint64_t value);

// Fetch translation for a host-side loader. Does not raise; reports the fault.
// MPRV does not apply. `pa` is valid only when both faults are false.
bool ideal_translate_fetch(const IdealHart* hart, uint64_t pc, unsigned len,
                           uint64_t* pa, bool* page_fault, bool* access_fault);

void ideal_uart_reset();
// MEIP/SEIP bits currently asserted by the UART's PLIC line.
uint64_t ideal_device_mip();

#endif
