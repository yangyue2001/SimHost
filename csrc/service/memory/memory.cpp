#include "service/memory/memory.h"

#include <elf.h>
#include <sys/stat.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "session/options.h"

#ifdef WITH_DRAMSIM3
#include "cosimulation.h"
#include "dramsim3_paths.h"
#endif

namespace {

// Offsets that are not 8-byte aligned, or that fall past words_, are rejected.
constexpr uint64_t kDefaultRamBytes = 256ull << 20;
// ELF p_paddr is a guest physical address. Offset 0 in words_ is this base.

// DPI entry points are free functions, so they find the active service here.
// init() publishes it and finish() clears it. A second service is refused.
MemoryService* current_service = nullptr;

// Accept 256, 256B, 256KB, 256MB, or 256GB. The result is a byte count and
// must be a multiple of one word.
bool parse_ram_size(const char* text, uint64_t* out) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    std::fprintf(stderr, "invalid numeric value for --ram-size\n");
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || value == 0) {
    std::fprintf(stderr, "invalid numeric value for --ram-size\n");
    return false;
  }
  uint64_t scale = 1;
  if (std::strcmp(end, "") == 0 || std::strcmp(end, "B") == 0 ||
      std::strcmp(end, "b") == 0) {
    scale = 1;
  } else if (std::strcmp(end, "KB") == 0 || std::strcmp(end, "kb") == 0) {
    scale = 1024ull;
  } else if (std::strcmp(end, "MB") == 0 || std::strcmp(end, "mb") == 0) {
    scale = 1024ull * 1024ull;
  } else if (std::strcmp(end, "GB") == 0 || std::strcmp(end, "gb") == 0) {
    scale = 1024ull * 1024ull * 1024ull;
  } else {
    std::fprintf(stderr, "invalid ram size unit %s\n", end);
    return false;
  }
  if (value > UINT64_MAX / scale) {
    std::fprintf(stderr, "--ram-size is too large\n");
    return false;
  }
  uint64_t bytes = value * scale;
  if (bytes % 8 != 0) {
    std::fprintf(stderr, "--ram-size must be a multiple of 8 bytes\n");
    return false;
  }
  *out = bytes;
  return true;
}

bool host_little_endian() {
  uint16_t probe = 1;
  return *reinterpret_cast<uint8_t*>(&probe) == 1;
}

bool read_file(const char* path, std::vector<uint8_t>* out) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot open image %s\n", path);
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    std::fprintf(stderr, "cannot size image %s\n", path);
    return false;
  }
  long size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    std::fprintf(stderr, "cannot size image %s\n", path);
    return false;
  }
  out->resize(static_cast<size_t>(size));
  size_t got = 0;
  if (size > 0) {
    got = std::fread(out->data(), 1, out->size(), file);
  }
  std::fclose(file);
  if (got != out->size()) {
    std::fprintf(stderr, "cannot read image %s\n", path);
    return false;
  }
  return true;
}

// Byte 0 of the guest word is bits [7:0]. The host must be little-endian so
// a uint64_t in words_ has that layout.
bool install_span(std::vector<uint64_t>* words, uint64_t offset,
                  const uint8_t* src, uint64_t filesz, uint64_t memsz) {
  if (memsz < filesz) {
    return false;
  }
  uint64_t bytes = static_cast<uint64_t>(words->size()) * 8ull;
  if (offset > bytes || memsz > bytes - offset) {
    std::fprintf(stderr, "image does not fit functional RAM\n");
    return false;
  }
  uint8_t* dst = reinterpret_cast<uint8_t*>(words->data()) + offset;
  if (filesz != 0) {
    std::memcpy(dst, src, static_cast<size_t>(filesz));
  }
  if (memsz > filesz) {
    std::memset(dst + filesz, 0, static_cast<size_t>(memsz - filesz));
  }
  return true;
}

bool file_range(size_t size, uint64_t offset, uint64_t length) {
  return offset <= size && length <= size - offset;
}

bool load_elf(const std::vector<uint8_t>& file, std::vector<uint64_t>* words) {
  if (file.size() < sizeof(Elf64_Ehdr)) {
    std::fprintf(stderr, "truncated ELF header\n");
    return false;
  }
  Elf64_Ehdr eh;
  std::memcpy(&eh, file.data(), sizeof(eh));
  if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_machine != EM_RISCV ||
      eh.e_phnum == 0 || eh.e_phentsize < sizeof(Elf64_Phdr) ||
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
    if (ph.p_type != PT_LOAD || ph.p_memsz == 0) {
      continue;
    }
    if (ph.p_filesz > ph.p_memsz ||
        !file_range(file.size(), ph.p_offset, ph.p_filesz)) {
      std::fprintf(stderr, "ELF segment %u is outside the file\n", index);
      return false;
    }
    if (ph.p_paddr < kFunctionalRamBase) {
      std::fprintf(stderr, "ELF segment %u is below RAM base\n", index);
      return false;
    }
    const uint8_t* src = file.data() + ph.p_offset;
    if (!install_span(words, ph.p_paddr - kFunctionalRamBase, src, ph.p_filesz,
                      ph.p_memsz)) {
      return false;
    }
    any = true;
  }
  if (!any) {
    std::fprintf(stderr, "ELF image has no loadable segment\n");
    return false;
  }
  return true;
}

bool make_directory(const char* path) {
  if (path == nullptr || *path == '\0') {
    return false;
  }
  if (mkdir(path, 0755) == 0 || errno == EEXIST) {
    return true;
  }
  std::fprintf(stderr, "cannot create DRAMSim3 output directory %s\n", path);
  return false;
}

}  // namespace

// words_ stays empty until init(). dram_ stays null when DRAMSim3 is absent.
MemoryService::MemoryService()
    : ram_bytes_(kDefaultRamBytes), dram_(nullptr), failed_(false) {}

MemoryService::~MemoryService() { finish(STATE_RUNNING); }

const char* MemoryService::name() const { return "memory"; }

// context is the MemoryService passed to OptionSet::add_value(). Parsing
// only stores the value. The model is created later by init().
bool MemoryService::apply_ram_size(void* context, const char* value) {
  auto* service = static_cast<MemoryService*>(context);
  return parse_ram_size(value, &service->ram_bytes_);
}

bool MemoryService::apply_ini(void* context, const char* value) {
#ifndef WITH_DRAMSIM3
  (void)context;
  (void)value;
  std::fprintf(stderr,
               "DRAMSim3 is not enabled, but --dramsim3-ini is specified\n");
  return false;
#else
  auto* service = static_cast<MemoryService*>(context);
  service->ini_ = value == nullptr ? "" : value;
  return true;
#endif
}

bool MemoryService::apply_outdir(void* context, const char* value) {
#ifndef WITH_DRAMSIM3
  (void)context;
  (void)value;
  std::fprintf(stderr,
               "DRAMSim3 is not enabled, but --dramsim3-outdir is specified\n");
  return false;
#else
  auto* service = static_cast<MemoryService*>(context);
  service->outdir_ = value == nullptr ? "" : value;
  return true;
#endif
}

bool MemoryService::apply_image(void* context, const char* value) {
  auto* service = static_cast<MemoryService*>(context);
  if (value == nullptr || value[0] == '\0') {
    std::fprintf(stderr, "--image needs a path\n");
    return false;
  }
  service->image_ = value;
  return true;
}

// Copy --image into words_. Called after the vector exists and before the
// service is published, so a bad file never becomes the active RAM.
bool MemoryService::load_image() {
  if (image_.empty()) {
    return true;
  }
  if (!host_little_endian()) {
    std::fprintf(stderr, "image loader expects a little-endian host\n");
    return false;
  }
  std::vector<uint8_t> file;
  if (!read_file(image_.c_str(), &file)) {
    return false;
  }
  bool elf = file.size() >= 4 && file[0] == 0x7f && file[1] == 'E' &&
             file[2] == 'L' && file[3] == 'F';
  if (elf) {
    if (!load_elf(file, &words_)) {
      return false;
    }
  } else if (!install_span(&words_, 0, file.data(), file.size(), file.size())) {
    return false;
  }
  std::printf("functional RAM image: %s\n", image_.c_str());
  return true;
}

uint64_t memory_ram_bytes() {
  if (current_service == nullptr) {
    return 0;
  }
  return current_service->ram_bytes();
}

uint8_t* MemoryService::ram_data() {
  if (words_.empty()) {
    return nullptr;
  }
  return reinterpret_cast<uint8_t*>(words_.data());
}

uint8_t* memory_ram_data() {
  if (current_service == nullptr) {
    return nullptr;
  }
  return current_service->ram_data();
}

// The session does not know these names. They become --ram-size and the two
// DRAMSim3 long options. A smoke build rejects the DRAMSim3 options here.
void MemoryService::register_options(OptionSet& options) {
  options.add_value("ram-size", &MemoryService::apply_ram_size, this,
                    "functional RAM size, for example 256MB");
  options.add_value("image", &MemoryService::apply_image, this,
                    "ELF or raw binary loaded into functional RAM");
  options.add_value("dramsim3-ini", &MemoryService::apply_ini, this,
                    "DRAMSim3 ini file; requires the dram build");
  options.add_value("dramsim3-outdir", &MemoryService::apply_outdir, this,
                    "DRAMSim3 output directory; requires the dram build");
}

bool MemoryService::init(const SimContext&) {
  // The functional RAM exists in every build. DRAMSim3 is only the timing
  // backend, and only when this binary was built with it.
#ifdef WITH_DRAMSIM3
  // Both the functional RAM and the timing model are created before the loop.
  const char* config = ini_.empty() ? DRAMSIM3_CONFIG : ini_.c_str();
  const char* outdir = outdir_.empty() ? DRAMSIM3_OUTDIR : outdir_.c_str();
  FILE* file = std::fopen(config, "r");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot open DRAMSim3 config file: %s\n", config);
    return false;
  }
  std::fclose(file);
  if (!make_directory(outdir)) {
    return false;
  }
  dram_ = new ComplexCoDRAMsim3(config, outdir);
  std::printf("DRAMSim3 config: %s\n", config);
  std::printf("DRAMSim3 outdir: %s\n", outdir);
#endif
  if (ram_bytes_ / 8 == 0) {
    std::fprintf(stderr, "--ram-size is too small\n");
    return false;
  }
  words_.assign(static_cast<size_t>(ram_bytes_ / 8), 0);
  if (!load_image()) {
    words_.clear();
    return false;
  }
  current_service = this;
  std::printf("functional RAM: %" PRIu64 " bytes\n", ram_bytes_);
  return true;
}

int MemoryService::on_posedge(SimContext&) {
  if (failed_) {
    return STATE_ABORT;
  }
#ifdef WITH_DRAMSIM3
  // Verilog already called the DPI functions during the rising eval. Tick
  // afterwards, so a request submitted in this cycle cannot complete in it.
  if (dram_ != nullptr) {
    static_cast<CoDRAMsim3*>(dram_)->tick();
  }
#else
  // Publish the requests accepted during this rising eval. The same eval
  // already polled memory_response, so it cannot see them until next cycle.
  ready_.insert(ready_.end(), held_.begin(), held_.end());
  held_.clear();
#endif
  return STATE_RUNNING;
}

// Safe to call from the destructor and from the session. The second call
// deletes a null model and clears an empty vector.
void MemoryService::finish(int) {
  if (current_service == this) {
    current_service = nullptr;
  }
#ifdef WITH_DRAMSIM3
  delete static_cast<CoDRAMsim3*>(dram_);
#endif
  dram_ = nullptr;
  held_.clear();
  ready_.clear();
  words_.clear();
}

// Index is offset / 8. The argument is already a byte offset, not a physical
// address. Failure is reported by the caller.
bool MemoryService::word_index(uint64_t offset, uint64_t* index) const {
  if (words_.empty() || offset % 8 != 0) {
    return false;
  }
  uint64_t word = offset / 8;
  if (word >= words_.size()) {
    return false;
  }
  *index = word;
  return true;
}

// Merge selected bytes into the old word. A zero strobe bit keeps that byte.
// This updates functional data at request time, not when the response returns.
void MemoryService::write_word(uint64_t offset, uint64_t data,
                               unsigned char strobe) {
  uint64_t index = 0;
  if (!word_index(offset, &index)) {
    std::fprintf(stderr, "memory write out of range: offset 0x%" PRIx64 "\n",
                 offset);
    failed_ = true;
    return;
  }
  uint64_t mask = 0;
  for (int byte = 0; byte < 8; ++byte) {
    if ((strobe & (1u << byte)) != 0) {
      mask |= 0xffull << (8 * byte);
    }
  }
  words_[static_cast<size_t>(index)] =
      (words_[static_cast<size_t>(index)] & ~mask) | (data & mask);
}

uint64_t MemoryService::read_word(uint64_t offset) {
  uint64_t index = 0;
  if (!word_index(offset, &index)) {
    std::fprintf(stderr, "memory read out of range: offset 0x%" PRIx64 "\n",
                 offset);
    failed_ = true;
    return 0;
  }
  uint64_t data = words_[static_cast<size_t>(index)];
  if (offset >= 0xefc0ull && offset < 0xf040ull) {
    static int logs = 0;
    if (logs < 16) {
      std::fprintf(stderr, "MEMREAD off=%" PRIx64 " data=%016" PRIx64 "\n",
                   offset, data);
      logs++;
    }
  }
  return data;
}

namespace {

#ifdef WITH_DRAMSIM3
struct DramMeta {
  uint32_t id;
};
#endif

}  // namespace

// Hand the byte offset and id to DRAMSim3. DRAMSim3 uses that offset for
// its bank and row map; it does not store the data. The request owns DramMeta
// until take_response() deletes it. Return 0 when the queue will not take it.
unsigned char MemoryService::accept(uint64_t offset, unsigned int id,
                                    unsigned char is_write) {
  uint64_t index = 0;
  if (offset >= 0xefc0ull && offset < 0xf080ull) {
    static int logs = 0;
    if (logs < 8) {
      std::fprintf(stderr, "MEMREQ off=%" PRIx64 " id=%u wr=%u\n", offset, id,
                   is_write);
      logs++;
    }
  }
  if (!word_index(offset, &index)) {
    std::fprintf(stderr, "memory request out of range: offset 0x%" PRIx64 "\n",
                 offset);
    failed_ = true;
    return 0;
  }

#ifdef WITH_DRAMSIM3
  if (dram_ == nullptr) {
    return 0;
  }
  auto* dram = static_cast<CoDRAMsim3*>(dram_);
  if (!dram->will_accept(offset, is_write != 0)) {
    return 0;
  }
  auto* request = new CoDRAMRequest();
  auto* meta = new DramMeta();
  request->address = offset;
  request->is_write = is_write != 0;
  meta->id = id;
  request->meta = meta;
  if (!dram->add_request(request)) {
    delete meta;
    delete request;
    return 0;
  }
  return 1;
#else
  // Remember the id. The data stays in words_, and the response waits until
  // on_posedge() publishes this cycle's accepts.
  PlainResponse response;
  response.id = id;
  response.is_write = is_write;
  held_.push_back(response);
  return 1;
#endif
}

// Return id with bit 32 set, so id 0 is still distinct from "no response".
// Read responses also print how many DRAMSim3 cycles the access took.
uint64_t MemoryService::take_response(unsigned char is_write) {
#ifdef WITH_DRAMSIM3
  if (dram_ == nullptr) {
    return 0;
  }
  auto* dram = static_cast<CoDRAMsim3*>(dram_);
  CoDRAMResponse* response = is_write != 0 ? dram->check_write_response()
                                           : dram->check_read_response();
  if (response == nullptr) {
    return 0;
  }
  auto* meta = static_cast<DramMeta*>(response->req->meta);
  uint64_t value = static_cast<uint64_t>(meta->id) | (1ull << 32);
  if (is_write == 0 && response->resp_time >= response->req_time) {
    std::printf("DRAMSim3 read delay %" PRIu64 "\n",
                response->resp_time - response->req_time);
  }
  delete meta;
  delete response->req;
  delete response;
  return value;
#else
  for (std::vector<PlainResponse>::iterator it = ready_.begin();
       it != ready_.end(); ++it) {
    if (it->is_write != is_write) {
      continue;
    }
    uint64_t value = static_cast<uint64_t>(it->id) | (1ull << 32);
    ready_.erase(it);
    return value;
  }
  return 0;
#endif
}

// Verilog calls these during the rising eval. They only forward to the
// service published by init(). Names stay unmangled for DPI-C.
extern "C" unsigned char memory_request(uint64_t offset, unsigned int id,
                                        unsigned char is_write) {
  return current_service->accept(offset, id, is_write);
}

extern "C" void memory_write(uint64_t offset, uint64_t data,
                             unsigned char strobe) {
  current_service->write_word(offset, data, strobe);
}

extern "C" uint64_t memory_read(uint64_t offset) {
  return current_service->read_word(offset);
}

extern "C" uint64_t memory_response(unsigned char is_write) {
  return current_service->take_response(is_write);
}
