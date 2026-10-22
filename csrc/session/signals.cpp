#include "session/signals.h"

#include <signal.h>

#include <csignal>

namespace {
volatile std::sig_atomic_t pending_signal = 0;

void handle_signal(int) { pending_signal = 1; }
}  // namespace

void install_signal_handlers() {
  struct sigaction action;
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

bool signal_requested() { return pending_signal != 0; }
