#include "session/registry.h"

#include "service/memory/memory.h"

std::vector<std::unique_ptr<SimService>> make_services() {
  std::vector<std::unique_ptr<SimService>> services;
  services.push_back(std::make_unique<MemoryService>());
  return services;
}
