#include "session/registry.h"

#include "service/lightsss/lightsss.h"
#include "service/memory/memory.h"

std::vector<std::unique_ptr<SimService>> make_services() {
  std::vector<std::unique_ptr<SimService>> services;
  services.push_back(std::make_unique<MemoryService>());
  // After memory, so a checkpoint is taken after this cycle's DRAM tick.
  services.push_back(std::make_unique<LightsssService>());
  return services;
}
