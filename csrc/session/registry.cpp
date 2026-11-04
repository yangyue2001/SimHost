#include "session/registry.h"

#include "service/difftest/difftest.h"
#include "service/ideal/ideal.h"
#include "service/lightsss/lightsss.h"
#include "service/memory/memory.h"
#include "service/mmu/mmu.h"

std::vector<std::unique_ptr<SimService>> make_services() {
  std::vector<std::unique_ptr<SimService>> services;
  services.push_back(std::make_unique<MemoryService>());
  // After memory, so a page-table read sees the functional RAM.
  // No on_posedge work. Verilog calls it during eval.
  services.push_back(std::make_unique<IdealMmu>());
  // After memory, so a load or store during eval sees this cycle's RAM.
  // No on_posedge work. Verilog calls ideal_backend_inst during eval.
  services.push_back(std::make_unique<IdealBackend>());
  // After memory, so a commit sees this cycle's memory tick first.
  // Probe DPI pushes into this service during eval.
  services.push_back(std::make_unique<DifftestService>());
  // After difftest, so an instruction mismatch can wake a checkpoint.
  services.push_back(std::make_unique<LightsssService>());
  return services;
}
