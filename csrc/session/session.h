#ifndef SIM_SESSION_SESSION_H
#define SIM_SESSION_SESSION_H

#include <cstdint>
#include <memory>
#include <vector>

#include "service/service.h"
#include "session/args.h"

enum class SessionStartup {
  kReady,
  kHelp,
  kError,
};

struct SessionResult {
  bool started = false;
  int trap = 0;
  uint64_t cycle = 0;
};

class Session {
 public:
  Session();

  SessionStartup open(int argc, char** argv);
  SessionResult run();

 private:
  std::vector<std::unique_ptr<SimService>> services_;
  SessionArgs args_;
  bool ready_;
};

const char* trap_label(int trap);
int trap_exit_code(int trap);

#endif
