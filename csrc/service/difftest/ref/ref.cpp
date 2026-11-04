#include "service/difftest/ref/ref.h"

#include <elf.h>

#include <dlfcn.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "service/memory/memory.h"

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

bool read_file(const char* path, std::vector<uint8_t>* out) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot read image %s\n", path);
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    std::fprintf(stderr, "cannot read image %s\n", path);
    return false;
  }
  long size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    std::fprintf(stderr, "cannot read image %s\n", path);
    return false;
  }
  out->resize(static_cast<size_t>(size));
  if (size > 0 &&
      std::fread(out->data(), 1, out->size(), file) != out->size()) {
    std::fclose(file);
    std::fprintf(stderr, "cannot read image %s\n", path);
    return false;
  }
  std::fclose(file);
  return true;
}

bool file_range(size_t size, uint64_t offset, uint64_t length) {
  return offset <= size && length <= size - offset;
}

bool in_ram(uint64_t base, uint64_t ram_size, uint64_t addr, uint64_t n) {
  if (n == 0) {
    return true;
  }
  if (addr < base) {
    return false;
  }
  uint64_t offset = addr - base;
  return offset <= ram_size && n <= ram_size - offset;
}

}  // namespace

RefLibrary::RefLibrary()
    : handle_(nullptr),
      init_(nullptr),
      memcpy_(nullptr),
      regcpy_(nullptr),
      exec_(nullptr),
      raise_trap_(nullptr),
      state_size_(nullptr),
      ram_base_(nullptr),
      ram_size_(nullptr) {}

RefLibrary::~RefLibrary() { close(); }

bool RefLibrary::open(const char* path) {
  close();
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
  init_ = reinterpret_cast<void (*)(void)>(load_symbol(handle_, "difftest_init"));
  memcpy_ = reinterpret_cast<void (*)(uint64_t, void*, size_t, int)>(
      load_symbol(handle_, "difftest_memcpy"));
  regcpy_ = reinterpret_cast<void (*)(void*, int)>(
      load_symbol(handle_, "difftest_regcpy"));
  exec_ = reinterpret_cast<void (*)(uint64_t)>(load_symbol(handle_, "difftest_exec"));
  raise_trap_ = reinterpret_cast<void (*)(uint64_t, uint64_t)>(
      load_symbol(handle_, "difftest_raise_trap"));
  state_size_ = reinterpret_cast<uint64_t (*)(void)>(
      load_symbol(handle_, "difftest_state_size"));
  ram_base_ = reinterpret_cast<uint64_t (*)(void)>(
      load_symbol(handle_, "difftest_ram_base"));
  ram_size_ = reinterpret_cast<uint64_t (*)(void)>(
      load_symbol(handle_, "difftest_ram_size"));
  if (init_ == nullptr || memcpy_ == nullptr || regcpy_ == nullptr ||
      exec_ == nullptr || raise_trap_ == nullptr || state_size_ == nullptr ||
      ram_base_ == nullptr || ram_size_ == nullptr) {
    close();
    return false;
  }
  if (state_size_() != sizeof(RvissRefState)) {
    std::fprintf(stderr,
                 "ref state is %" PRIu64 " bytes, simulator expects %zu\n",
                 state_size_(), sizeof(RvissRefState));
    close();
    return false;
  }
  init_();
  std::printf("difftest reference: %s\n", path);
  return true;
}

void RefLibrary::close() {
  init_ = nullptr;
  memcpy_ = nullptr;
  regcpy_ = nullptr;
  exec_ = nullptr;
  raise_trap_ = nullptr;
  state_size_ = nullptr;
  ram_base_ = nullptr;
  ram_size_ = nullptr;
  if (handle_ != nullptr) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

bool RefLibrary::load_image() {
  const char* path = memory_image_path();
  if (path == nullptr || path[0] == '\0') {
    return true;
  }
  std::vector<uint8_t> file;
  if (!read_file(path, &file)) {
    return false;
  }
  const uint64_t base = ram_base_();
  const uint64_t ram_size = ram_size_();
  bool elf = file.size() >= 4 && file[0] == 0x7f && file[1] == 'E' &&
             file[2] == 'L' && file[3] == 'F';
  if (!elf) {
    if (!in_ram(base, ram_size, base, file.size())) {
      std::fprintf(stderr, "image does not fit in the reference RAM\n");
      return false;
    }
    memcpy_(base, file.data(), file.size(), DIFFTEST_TO_REF);
    std::printf("difftest reference: ISS image %s\n", path);
    return true;
  }
  if (file.size() < sizeof(Elf64_Ehdr)) {
    std::fprintf(stderr, "truncated ELF header\n");
    return false;
  }
  Elf64_Ehdr eh;
  std::memcpy(&eh, file.data(), sizeof(eh));
  if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
      eh.e_machine != EM_RISCV || eh.e_phnum == 0 ||
      eh.e_phentsize < sizeof(Elf64_Phdr) ||
      !file_range(file.size(), eh.e_phoff,
                  static_cast<uint64_t>(eh.e_phnum) * eh.e_phentsize)) {
    std::fprintf(stderr, "unsupported RV64 ELF image\n");
    return false;
  }
  bool any = false;
  for (unsigned index = 0; index < eh.e_phnum; ++index) {
    Elf64_Phdr ph;
    std::memcpy(&ph, file.data() + eh.e_phoff + index * eh.e_phentsize,
                sizeof(ph));
    if (ph.p_type != PT_LOAD || ph.p_filesz == 0) {
      continue;
    }
    if (ph.p_filesz > ph.p_memsz ||
        !file_range(file.size(), ph.p_offset, ph.p_filesz) ||
        !in_ram(base, ram_size, ph.p_paddr, ph.p_filesz)) {
      std::fprintf(stderr, "ELF segment %u is outside the reference RAM\n",
                   index);
      return false;
    }
    memcpy_(ph.p_paddr, file.data() + ph.p_offset, ph.p_filesz, DIFFTEST_TO_REF);
    any = true;
  }
  if (!any) {
    std::fprintf(stderr, "ELF image has no loadable segment\n");
    return false;
  }
  std::printf("difftest reference: ISS image %s\n", path);
  return true;
}

void RefLibrary::regcpy(RvissRefState* state, int direction) {
  regcpy_(state, direction);
}

void RefLibrary::exec(uint64_t n) { exec_(n); }

void RefLibrary::raise_trap(uint64_t cause, uint64_t tval) {
  raise_trap_(cause, tval);
}
