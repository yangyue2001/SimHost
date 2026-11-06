#include "session/registry.h"

#include "service/difftest/difftest.h"
#include "service/lightsss/lightsss.h"
#include "service/memory/memory.h"

std::vector<std::unique_ptr<SimService>> make_services() {
  std::vector<std::unique_ptr<SimService>> services;
  services.push_back(std::make_unique<MemoryService>());
  // After memory, so a commit sees this cycle's memory tick first.
  // Probe DPI pushes into this service during eval.
  services.push_back(std::make_unique<DifftestService>());
  // After difftest, so an instruction mismatch can wake a checkpoint.
  services.push_back(std::make_unique<LightsssService>());
  return services;
}
