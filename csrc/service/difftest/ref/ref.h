#ifndef SIM_SERVICE_DIFFTEST_REF_H
#define SIM_SERVICE_DIFFTEST_REF_H

#include <cstdint>

#include "service/difftest/ref/ref_abi.h"

// One dlopen'd reference model. The session never sees the file name.
class RefLibrary {
 public:
  RefLibrary();
  ~RefLibrary();

  RefLibrary(const RefLibrary&) = delete;
  RefLibrary& operator=(const RefLibrary&) = delete;

  // dlopen path, check the ABI, and call difftest_init. Prints the reason
  // and returns false when the file or a required symbol is missing.
  bool open(const char* path);
  void close();
  bool opened() const { return handle_ != nullptr; }

  // Copy --image into the reference RAM with difftest_memcpy.
  // No image leaves the reference at the state from difftest_init.
  bool load_image();
  void regcpy(RvissRefState* state, int direction);
  void exec(uint64_t n);
  void raise_trap(uint64_t cause, uint64_t tval);

 private:
  void* handle_;
  void (*init_)(void);
  void (*memcpy_)(uint64_t, void*, size_t, int);
  void (*regcpy_)(void*, int);
  void (*exec_)(uint64_t);
  void (*raise_trap_)(uint64_t, uint64_t);
  uint64_t (*state_size_)(void);
  uint64_t (*ram_base_)(void);
  uint64_t (*ram_size_)(void);
};

#endif
