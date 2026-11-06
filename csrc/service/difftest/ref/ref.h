#ifndef SIM_SERVICE_DIFFTEST_REF_H
#define SIM_SERVICE_DIFFTEST_REF_H

#include <difftest.h>

#include <cstddef>
#include <cstdint>

// One dlopen'd reference model. The library owns the CPU and its RAM.
// direction is DIFFTEST_TO_DUT (read from the reference) or DIFFTEST_TO_REF.
class RefLibrary {
 public:
  RefLibrary();
  ~RefLibrary();

  RefLibrary(const RefLibrary&) = delete;
  RefLibrary& operator=(const RefLibrary&) = delete;

  // dlopen path and call difftest_init. RvissRefState comes from difftest.h.
  bool open(const char* path);
  void close();
  bool opened() const { return handle_ != nullptr; }

  // direction is DIFFTEST_TO_DUT or DIFFTEST_TO_REF. The ref ABI uses bool.
  void memcpy(uint64_t addr, void* buf, size_t n, bool direction);
  void regcpy(RvissRefState* state, bool direction);
  void exec(uint64_t n);
  // cause is mcause. Bit 63 set is an interrupt; clear is a synchronous
  // exception. The cause code itself must be below 64.
  void raise_trap(uint64_t cause, uint64_t tval);

 private:
  void* handle_;
  void (*memcpy_)(uint64_t, void*, size_t, bool);
  void (*regcpy_)(void*, bool);
  void (*exec_)(uint64_t);
  void (*raise_trap_)(uint64_t, uint64_t);
};

#endif
