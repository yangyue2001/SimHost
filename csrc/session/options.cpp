#include "session/options.h"

namespace {

bool SetFlag(void* context, const char* /*value*/) {
  bool* slot = static_cast<bool*>(context);
  *slot = true;
  return true;
}

}  // namespace

void OptionSet::add_flag(const char* name, bool* slot, const char* help) {
  RegisteredOption option;
  option.name = name;
  option.help = help == nullptr ? "" : help;
  option.takes_value = false;
  option.apply = SetFlag;
  option.context = slot;
  items_.push_back(option);
}

void OptionSet::add_value(const char* name,
                          bool (*apply)(void* context, const char* value),
                          void* context, const char* help) {
  RegisteredOption option;
  option.name = name;
  option.help = help == nullptr ? "" : help;
  option.takes_value = true;
  option.apply = apply;
  option.context = context;
  items_.push_back(option);
}
