#include "service/difftest/ref/ref.h"

#include <dlfcn.h>

#include <cinttypes>
#include <cstdio>

namespace {

void* load_symbol(void* handle, const char* name) {
  dlerror();
  void* symbol = dlsym(handle, name);
  const char* error = dlerror();
  if (symbol == nullptr || error != nullptr) {
    std::fprintf(stderr, "ref is missing %s: %s\n", name,
                 error != nullptr ? error : "null symbol");
    return nullptr;
  }
  return symbol;
}

}  // namespace

RefLibrary::RefLibrary()
    : handle_(nullptr),
      memcpy_(nullptr),
      regcpy_(nullptr),
      exec_(nullptr),
      raise_trap_(nullptr) {}

RefLibrary::~RefLibrary() { close(); }

bool RefLibrary::open(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    std::fprintf(stderr, "--diff needs a reference library path\n");
    return false;
  }
  // The reference does not parse argv. DEEPBIND is not used.
  handle_ = dlopen(path, RTLD_LAZY);
  if (handle_ == nullptr) {
    std::fprintf(stderr, "cannot open ref %s: %s\n", path, dlerror());
    return false;
  }

  auto init =
      reinterpret_cast<void (*)()>(load_symbol(handle_, "difftest_init"));
  memcpy_ = reinterpret_cast<void (*)(uint64_t, void*, size_t, bool)>(
      load_symbol(handle_, "difftest_memcpy"));
  regcpy_ = reinterpret_cast<void (*)(void*, bool)>(
      load_symbol(handle_, "difftest_regcpy"));
  exec_ = reinterpret_cast<void (*)(uint64_t)>(
      load_symbol(handle_, "difftest_exec"));
  raise_trap_ = reinterpret_cast<void (*)(uint64_t, uint64_t)>(
      load_symbol(handle_, "difftest_raise_trap"));

  if (init == nullptr || memcpy_ == nullptr || regcpy_ == nullptr ||
      exec_ == nullptr || raise_trap_ == nullptr) {
    close();
    return false;
  }

  init();
  std::printf("difftest reference: %s\n", path);
  return true;
}

void RefLibrary::close() {
  memcpy_ = nullptr;
  regcpy_ = nullptr;
  exec_ = nullptr;
  raise_trap_ = nullptr;
  if (handle_ != nullptr) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

void RefLibrary::memcpy(uint64_t addr, void* buf, size_t n, bool direction) {
  memcpy_(addr, buf, n, direction);
}

void RefLibrary::regcpy(RvissRefState* state, bool direction) {
  regcpy_(state, direction);
}

void RefLibrary::exec(uint64_t n) { exec_(n); }

void RefLibrary::raise_trap(uint64_t cause, uint64_t tval) {
  raise_trap_(cause, tval);
}
