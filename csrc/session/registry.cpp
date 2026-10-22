#include "session/registry.h"

std::vector<std::unique_ptr<SimService>> make_services() {
  // Phase 1 keeps the registry empty. Later services are added here only.
  return {};
}
