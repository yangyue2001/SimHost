#ifndef SIM_SERVICE_MEMORY_H
#define SIM_SERVICE_MEMORY_H

#include <cstdint>
#include <string>
#include <vector>

#include "service/service.h"

// One memory service for the simulated RAM.
//
// words_ is the functional memory: a write updates it immediately. DRAMSim3,
// when this binary is built with it, only decides when the response may be
// seen. The session calls the SimService methods and never sees a DPI name.
// Verilog calls the extern "C" functions in memory.cpp. Those functions have
// no object, so init() publishes this instance through a file-local pointer.
// Only one MemoryService can be active. A build without DRAMSim3 leaves that
// pointer unset, and the DPI functions then do nothing.
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
  // Allocate words_ and open DRAMSim3. Return false to stop before the loop.
  bool init(const SimContext& context) override;
  // After the rising eval, so this cycle's DPI calls have already run.
  // Advance the timing model by one cycle.
  int on_posedge(const SimContext& context) override;
  void finish() override;

  // Map an 8-byte-aligned byte offset into words_. Offset 0 is the first word.
  bool word_index(uint64_t offset, uint64_t* index) const;
  // Update the functional RAM now. strobe bit N enables byte N.
  void write_word(uint64_t offset, uint64_t data, unsigned char strobe);
  // Read one aligned word from the functional RAM.
  uint64_t read_word(uint64_t offset);
  // Queue a timed request. Return 1 if DRAMSim3 accepts it. This does not
  // move data; the caller writes or reads words_ separately.
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
  // CoDRAMsim3*, hidden so this header does not include DRAMSim3.
  void* dram_;
  // Sticky out-of-range access. The next on_posedge() returns STATE_ABORT.
  bool failed_;
};

#endif
