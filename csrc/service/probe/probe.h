#ifndef SIM_SERVICE_PROBE_H
#define SIM_SERVICE_PROBE_H

#include <cstdint>
#include <vector>

#include "service/service.h"

// One DPI sample. words follow the Chisel bundle source order, each widened
// to 64 bits. A Bool named valid is 1 here: the call is skipped when it is 0.
// kProbeMaxWords matches ProbeDpi.maxWords in the Scala probe package.
constexpr int kProbeNameLength = 64;
constexpr int kProbeMaxWords = 64;
constexpr int kProbeMaxPending = 4096;

struct ProbeSample {
  char name[kProbeNameLength];
  int nwords;
  uint64_t words[kProbeMaxWords];
};

// A service that owns a probe queue. push() appends one sample. The subclass
// reads pending() in on_posedge(), handles the names it knows, then clear().
// A full queue or a bad sample sets failed() and keeps the samples already
// accepted. Nothing is dropped. Memory, MMU, the ideal backend, and LightSSS
// do not inherit this.
class ProbeService : public SimService {
 public:
  ProbeService();

  // Called from DPI during eval(), before on_posedge().
  void push(const char* name, const uint64_t* words, int nwords);

 protected:
  const std::vector<ProbeSample>& pending() const { return pending_; }
  bool failed() const { return failed_; }
  const char* fail_reason() const { return reason_; }
  // Drop samples. A failure stays set so on_posedge() can still see it.
  void clear();
  // Drop samples and clear the failure. Used before the main loop.
  void discard();

 private:
  void fail(const char* reason);

  std::vector<ProbeSample> pending_;
  bool failed_;
  const char* reason_;
};

#endif
