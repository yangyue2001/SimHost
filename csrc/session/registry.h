#ifndef SIM_SESSION_REGISTRY_H
#define SIM_SESSION_REGISTRY_H

#include <memory>
#include <vector>

#include "service/service.h"

std::vector<std::unique_ptr<SimService>> make_services();

#endif
