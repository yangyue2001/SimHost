#ifndef SIM_PLATFORM_PLATFORM_H
#define SIM_PLATFORM_PLATFORM_H

#include <cstdint>
#include <memory>

class Platform {
 public:
  virtual ~Platform() = default;
  virtual void set_clock(unsigned level) = 0;
  virtual void set_reset(unsigned level) = 0;
  virtual void eval() = 0;
  virtual uint64_t exit_code() const = 0;
  virtual bool trace_compiled() const = 0;
  virtual const char* default_waveform_name() const = 0;
  virtual bool waveform_open(uint64_t start_time, const char* path) = 0;
  virtual void waveform_dump() = 0;
  virtual void close_waveform() = 0;
  virtual void set_assertions_enabled(bool enabled) = 0;
};

std::unique_ptr<Platform> create_platform(uint32_t seed);

#endif
