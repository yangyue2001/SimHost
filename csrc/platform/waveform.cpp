#include "platform/waveform.h"

#include <verilated.h>
#if defined(EMU_TRACE_FST)
#include <verilated_fst_c.h>
using TraceFile = VerilatedFstC;
#elif defined(EMU_TRACE_VCD)
#include <verilated_vcd_c.h>
using TraceFile = VerilatedVcdC;
#endif

#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
static TraceFile* as_file(void* file) { return static_cast<TraceFile*>(file); }
#endif

Waveform::Waveform() : file_(nullptr) {
#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
  file_ = new TraceFile();
#endif
}

Waveform::~Waveform() {
#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
  TraceFile* file = as_file(file_);
  if (file != nullptr) {
    file->close();
    delete file;
  }
#endif
  file_ = nullptr;
}

VerilatedTraceBaseC* Waveform::tracer() {
#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
  return as_file(file_);
#else
  return nullptr;
#endif
}

bool Waveform::open(const char* path) {
#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
  TraceFile* file = as_file(file_);
  if (file == nullptr || path == nullptr) {
    return false;
  }
  file->open(path);
  return true;
#else
  (void)path;
  return false;
#endif
}

void Waveform::dump(uint64_t time) {
#if defined(EMU_TRACE_FST) || defined(EMU_TRACE_VCD)
  TraceFile* file = as_file(file_);
  if (file == nullptr) {
    return;
  }
  file->dump(time);
#else
  (void)time;
#endif
}
