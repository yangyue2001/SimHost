#ifndef SIM_SESSION_OPTIONS_H
#define SIM_SESSION_OPTIONS_H

#include <string>
#include <vector>

struct RegisteredOption {
  std::string name;
  bool takes_value;
  bool (*apply)(void* context, const char* value);
  void* context;
};

class OptionSet {
 public:
  void add_flag(const char* name, bool* slot);
  void add_value(const char* name,
                 bool (*apply)(void* context, const char* value),
                 void* context);
  const std::vector<RegisteredOption>& items() const { return items_; }

 private:
  std::vector<RegisteredOption> items_;
};

#endif
