#ifndef SIM_SERVICE_DIFFTEST_REF_ABI_H
#define SIM_SERVICE_DIFFTEST_REF_ABI_H

/* The reference ABI is ISS include/difftest.h.
 *
 * Same shape as NEMU: difftest_init, difftest_memcpy, difftest_regcpy,
 * difftest_exec. ISS adds difftest_raise_trap because this reference has no
 * devices and does not sample interrupts itself. The simulator copies the
 * guest image with memcpy. A skipped instruction is written back with
 * regcpy. There is no difftest_load_image and no difftest_skip_one.
 *
 * RvissRefState is not NEMU's or XiangShan's CPU_state. sstatus is not a
 * field; it is the visible bits of mstatus.
 */
#include <difftest.h>

#endif
