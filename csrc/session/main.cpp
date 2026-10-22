#include <verilated.h>

#include <cinttypes>
#include <cstdio>

#include "session/session.h"
#include "session/signals.h"

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  install_signal_handlers();

  Session session;
  switch (session.open(argc, argv)) {
    case SessionStartup::kHelp:
      return 0;
    case SessionStartup::kError:
      return 1;
    case SessionStartup::kReady:
      break;
  }

  SessionResult result = session.run();
  if (!result.started) {
    return 1;
  }

  std::printf("%s at cycle %" PRIu64 "\n", trap_label(result.trap),
              result.cycle);
  return trap_exit_code(result.trap);
}
