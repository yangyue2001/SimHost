#ifndef SIM_PLATFORM_WAVEFORM_H
#define SIM_PLATFORM_WAVEFORM_H

#include <cstdint>

class VerilatedTraceBaseC;

class Waveform {
 public:
  Waveform();
  ~Waveform();

  Waveform(const Waveform&) = delete;
  Waveform& operator=(const Waveform&) = delete;

  VerilatedTraceBaseC* tracer();
  bool open(const char* path);
  void dump(uint64_t time);

 private:
  void* file_;
};

#endif
