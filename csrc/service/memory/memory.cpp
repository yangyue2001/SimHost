#include "service/memory/memory.h"

#include <sys/stat.h>

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "session/options.h"

#ifdef WITH_DRAMSIM3
#include "cosimulation.h"
#include "dramsim3_paths.h"
#endif

namespace {

// Offsets that are not 8-byte aligned, or that fall past words_, are rejected.
constexpr uint64_t kDefaultRamBytes = 256ull << 20;

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

// The session does not know these names. They become --ram-size and the two
// DRAMSim3 long options. A smoke build rejects the DRAMSim3 options here.
void MemoryService::register_options(OptionSet& options) {
  options.add_value("ram-size", &MemoryService::apply_ram_size, this,
                    "functional RAM size, for example 256MB");
  options.add_value("dramsim3-ini", &MemoryService::apply_ini, this,
                    "DRAMSim3 ini file; requires the dram build");
  options.add_value("dramsim3-outdir", &MemoryService::apply_outdir, this,
                    "DRAMSim3 output directory; requires the dram build");
}

bool MemoryService::init(const SimContext&) {
  // The functional RAM exists in every build. DRAMSim3 is only the timing
  // backend, and only when this binary was built with it.
  if (current_service != nullptr) {
    std::fprintf(stderr, "only one memory service is supported\n");
    return false;
  }
#ifndef WITH_DRAMSIM3
  if (ram_bytes_ / 8 == 0) {
    std::fprintf(stderr, "--ram-size is too small\n");
    return false;
  }
  words_.assign(static_cast<size_t>(ram_bytes_ / 8), 0);
  current_service = this;
  std::printf("functional RAM: %" PRIu64 " bytes\n", ram_bytes_);
  return true;
#else
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
  if (ram_bytes_ / 8 == 0) {
    std::fprintf(stderr, "--ram-size is too small\n");
    return false;
  }
  words_.assign(static_cast<size_t>(ram_bytes_ / 8), 0);
  dram_ = new ComplexCoDRAMsim3(config, outdir);
  current_service = this;
  std::printf("DRAMSim3 config: %s\n", config);
  std::printf("DRAMSim3 outdir: %s\n", outdir);
  return true;
#endif
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
  return words_[static_cast<size_t>(index)];
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
  if (current_service == nullptr) {
    return 0;
  }
  return current_service->accept(offset, id, is_write);
}

extern "C" void memory_write(uint64_t offset, uint64_t data,
                             unsigned char strobe) {
  if (current_service == nullptr) {
    return;
  }
  current_service->write_word(offset, data, strobe);
}

extern "C" uint64_t memory_read(uint64_t offset) {
  if (current_service == nullptr) {
    return 0;
  }
  return current_service->read_word(offset);
}

extern "C" uint64_t memory_response(unsigned char is_write) {
  if (current_service == nullptr) {
    return 0;
  }
  return current_service->take_response(is_write);
}
