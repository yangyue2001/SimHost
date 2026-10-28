#ifndef SIM_SERVICE_MEMORY_H
#define SIM_SERVICE_MEMORY_H

#include <cstdint>
#include <string>
#include <vector>

#include "service/service.h"

// One memory service for the simulated RAM.
//
// words_ is the functional RAM: the bytes the simulated program reads and
// writes. DRAMSim3, when this binary is built with it, does not store those
// bytes. It only decides when a response becomes visible. Without DRAMSim3
// the same DPI still serves this RAM, and a response is visible one cycle
// after it is accepted. Verilog cannot tell which timing it is using.
//
// The session calls the SimService methods and never sees a DPI name.
// Verilog calls the extern "C" functions in memory.cpp. Those functions have
// no object, so init() publishes this instance through a file-local pointer.
// Only one MemoryService can be active.
//
// Every DPI address argument is a byte offset from the start of this RAM.
// Offset 0 is the first byte. The caller subtracts its own physical base
// before the call; this service does not know that base.
class MemoryService : public SimService {
 public:
  MemoryService();
  ~MemoryService() override;

  const char* name() const override;
  // Long options only. Parsing stores the strings; init() opens DRAMSim3.
  void register_options(OptionSet& options) override;
  // Allocate words_. Open DRAMSim3 only in the dram build.
  bool init(const SimContext& context) override;
  // After the rising eval. Tick DRAMSim3, or publish plain responses.
  int on_posedge(SimContext& context) override;
  void finish(int trap) override;

  // Map an 8-byte-aligned byte offset into words_. Offset 0 is the first word.
  bool word_index(uint64_t offset, uint64_t* index) const;
  // Update the functional RAM now. strobe bit N enables byte N.
  void write_word(uint64_t offset, uint64_t data, unsigned char strobe);
  // Read one aligned word from the functional RAM.
  uint64_t read_word(uint64_t offset);
  // Queue one request. Return 1 if it was accepted. DRAMSim3 may reject it
  // when its queue is full. Plain RAM accepts every in-range request. This
  // does not move data; the caller writes or reads words_ separately.
  unsigned char accept(uint64_t offset, unsigned int id,
                       unsigned char is_write);
  // Pop one finished response. Bit 32 means valid and bits [31:0] are the id.
  // Zero means there is no response. The payload is not included.
  uint64_t take_response(unsigned char is_write);

 private:
  // OptionSet stores a C function pointer, so these are static and receive
  // the MemoryService as void*.
  static bool apply_ram_size(void* context, const char* value);
  static bool apply_ini(void* context, const char* value);
  static bool apply_outdir(void* context, const char* value);

  // Empty unless the matching option was given. init() then uses its default.
  std::string ini_;
  std::string outdir_;
  uint64_t ram_bytes_;
  // Functional data, one element per 8 bytes. Empty until init().
  std::vector<uint64_t> words_;
  // One accepted request. is_write selects which response queue it belongs to.
  struct PlainResponse {
    uint32_t id;
    unsigned char is_write;
  };

  // CoDRAMsim3*, hidden so this header does not include DRAMSim3.
  // Null when this binary has no DRAMSim3, and the plain queues are used.
  void* dram_;
  // Accepted during this rising eval. memory_response cannot see these yet,
  // so a request never completes in the cycle that submitted it.
  std::vector<PlainResponse> held_;
  // Completed plain responses. on_posedge() moves held_ here.
  std::vector<PlainResponse> ready_;
  // Sticky out-of-range access. The next on_posedge() returns STATE_ABORT.
  bool failed_;
};

#endif
