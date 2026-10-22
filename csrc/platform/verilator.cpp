#include <verilated.h>

#include <cstdio>
#include <memory>

#include "VTop.h"
#include "platform/platform.h"
#include "platform/waveform.h"

namespace {

class VerilatorPlatform final : public Platform {
 public:
  VerilatorPlatform() : dut_(new VTop) {}

  ~VerilatorPlatform() override {
    close_waveform();
    delete dut_;
  }

  void set_clock(unsigned level) override { dut_->clock = level; }
  void set_reset(unsigned level) override { dut_->reset = level; }
  void eval() override { dut_->eval(); }
  uint64_t exit_code() const override { return dut_->exit; }

  bool trace_compiled() const override {
#if defined(EMU_TRACE_VCD) || defined(EMU_TRACE_FST)
    return true;
#else
    return false;
#endif
  }

  const char* default_waveform_name() const override {
#if defined(EMU_TRACE_FST)
    return "sim.fst";
#else
    return "sim.vcd";
#endif
  }

  bool waveform_open(uint64_t start_time, const char* path) override {
    if (!trace_compiled() || path == nullptr) {
      return false;
    }
    FILE* probe = std::fopen(path, "w");
    if (probe == nullptr) {
      return false;
    }
    std::fclose(probe);

    Verilated::traceEverOn(true);
    waveform_ = std::make_unique<Waveform>(start_time);
    if (waveform_->tracer() == nullptr) {
      waveform_.reset();
      return false;
    }
    dut_->trace(waveform_->tracer(), 99);
    return waveform_->open(path);
  }

  void waveform_dump() override {
    if (waveform_ != nullptr) {
      waveform_->dump();
    }
  }

  void close_waveform() override { waveform_.reset(); }

  void set_assertions_enabled(bool enabled) override {
    Verilated::assertOn(enabled);
  }

 private:
  VTop* dut_;
  std::unique_ptr<Waveform> waveform_;
};

}  // namespace

std::unique_ptr<Platform> create_platform(uint32_t seed) {
  if (seed == 0) {
    Verilated::randReset(0);
  } else {
    Verilated::randSeed(static_cast<int>(seed));
    Verilated::randReset(2);
  }
  Verilated::assertOn(false);
  return std::make_unique<VerilatorPlatform>();
}
