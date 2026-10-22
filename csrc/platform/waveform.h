#ifndef SIM_PLATFORM_WAVEFORM_H
#define SIM_PLATFORM_WAVEFORM_H

#include <cstdint>

class VerilatedTraceBaseC;

class Waveform {
 public:
  explicit Waveform(uint64_t start_time);
  ~Waveform();

  Waveform(const Waveform&) = delete;
  Waveform& operator=(const Waveform&) = delete;

  VerilatedTraceBaseC* tracer();
  bool open(const char* path);
  void dump();

 private:
  uint64_t time_;
  void* file_;
};

#endif
