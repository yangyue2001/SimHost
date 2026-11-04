#include <cstdint>

// Used when the elaborated Verilog does not export mmu_resp. A real export
// is a strong symbol and replaces this empty driver.
extern "C" void mmu_resp(uint64_t ppn, unsigned char page_fault,
                         unsigned char access_fault) __attribute__((weak));

extern "C" void mmu_resp(uint64_t, unsigned char, unsigned char) {}
