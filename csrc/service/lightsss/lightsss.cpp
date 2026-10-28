#include "service/lightsss/lightsss.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <new>

#include "session/options.h"

namespace {

constexpr int kSlotCount = 2;
constexpr uint64_t kStepForward = 100;
constexpr uint64_t kDefaultIntervalMs = 10000;

struct ShareBody {
  std::atomic<uint32_t> flag;
  std::atomic<uint32_t> notgood;
  std::atomic<uint64_t> end_cycle;
  std::atomic<int> oldest;
};

uint64_t now_ms() {
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000ull +
         static_cast<uint64_t>(now.tv_nsec) / 1000000ull;
}

bool parse_u64(const char* text, const char* option, uint64_t* out) {
  if (text == nullptr || *text == '\0' || *text == '-') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  char* end = nullptr;
  errno = 0;
  unsigned long long value = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    std::fprintf(stderr, "invalid numeric value for --%s\n", option);
    return false;
  }
  *out = static_cast<uint64_t>(value);
  return true;
}

void flush_stdio() {
  std::fflush(stdout);
  std::fflush(stderr);
  std::cout.flush();
  std::cerr.flush();
}

void wait_pid(int pid) {
  while (waitpid(pid, nullptr, 0) < 0) {
    if (errno != EINTR) {
      break;
    }
  }
}

}  // namespace

struct LightsssShare : ShareBody {};

LightsssService::LightsssService()
    : enabled_(false),
      fail_enabled_(false),
      is_child_(false),
      have_forked_(false),
      interval_ms_(kDefaultIntervalMs),
      fail_cycle_(0),
      last_cycle_(0),
      last_fork_ms_(0),
      share_(nullptr) {}

LightsssService::~LightsssService() {
  if (!is_child_) {
    clear_children();
  }
  if (share_ != nullptr) {
    munmap(share_, sizeof(LightsssShare));
    share_ = nullptr;
  }
}

const char* LightsssService::name() const { return "lightsss"; }

bool LightsssService::defer_waveform() const { return enabled_; }

bool LightsssService::apply_interval(void* context, const char* value) {
  auto* service = static_cast<LightsssService*>(context);
  uint64_t seconds = 0;
  if (!parse_u64(value, "fork-interval", &seconds) || seconds == 0 ||
      seconds > 24ull * 60ull * 60ull) {
    std::fprintf(stderr, "--fork-interval must be 1..86400 seconds\n");
    return false;
  }
  service->interval_ms_ = seconds * 1000ull;
  return true;
}

bool LightsssService::apply_fail_at(void* context, const char* value) {
  auto* service = static_cast<LightsssService*>(context);
  uint64_t cycle = 0;
  if (!parse_u64(value, "fork-fail-at", &cycle)) {
    return false;
  }
  service->fail_enabled_ = true;
  service->fail_cycle_ = cycle;
  return true;
}

void LightsssService::register_options(OptionSet& options) {
  options.add_flag("fork", &enabled_, "fork LightSSS checkpoints");
  options.add_value("fork-interval", &LightsssService::apply_interval, this,
                    "seconds between checkpoints (default 10)");
  options.add_value("fork-fail-at", &LightsssService::apply_fail_at, this,
                    "parent-only abort cycle, used to test a checkpoint");
}

bool LightsssService::init(const SimContext&) {
  if (!enabled_ && !fail_enabled_) {
    return true;
  }
  // Line-buffer even when stdout is redirected, so a fork does not duplicate
  // text that the parent has already printed.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  flush_stdio();
  if (!enabled_) {
    return true;
  }
  void* memory = mmap(nullptr, sizeof(LightsssShare), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) {
    std::perror("LightSSS mmap");
    return false;
  }
  share_ = new (memory) LightsssShare();
  share_->flag.store(0);
  share_->notgood.store(0);
  share_->end_cycle.store(0);
  share_->oldest.store(0);
  std::printf("LightSSS enabled, interval %" PRIu64 " ms, step %" PRIu64 "\n",
              interval_ms_, kStepForward);
  std::printf("LightSSS parent will not dump a waveform\n");
  flush_stdio();
  return true;
}

int LightsssService::child_cycle(const SimContext& context) {
  uint64_t end = share_->end_cycle.load(std::memory_order_relaxed);
  if (context.cycle == end) {
    std::printf("LightSSS reached abort cycle %" PRIu64 "\n", end);
    flush_stdio();
  }
  if (context.cycle == end + kStepForward) {
    std::printf("LightSSS step forward done at cycle %" PRIu64 "\n",
                context.cycle);
    flush_stdio();
    return STATE_ABORT;
  }
  return STATE_RUNNING;
}

bool LightsssService::spawn(SimContext& context) {
  while (static_cast<int>(slots_.size()) >= kSlotCount) {
    int oldest = slots_.back();
    slots_.pop_back();
    kill(oldest, SIGKILL);
    wait_pid(oldest);
  }

  flush_stdio();
  pid_t pid = fork();
  if (pid < 0) {
    std::perror("LightSSS fork");
    return false;
  }
  if (pid > 0) {
    slots_.push_front(pid);
    std::printf("LightSSS checkpoint pid %d at cycle %" PRIu64 "\n", pid,
                context.cycle);
    flush_stdio();
    return true;
  }

  is_child_ = true;
  slots_.clear();
  while (share_->flag.load(std::memory_order_acquire) == 0) {
    usleep(1000);
  }
  if (share_->notgood.load(std::memory_order_relaxed) == 0) {
    _exit(0);
  }
  if (share_->oldest.load(std::memory_order_relaxed) != getpid()) {
    std::fprintf(stderr, "LightSSS non-oldest child %d woke\n", getpid());
    _exit(1);
  }
  std::printf("LightSSS child %d resumed at cycle %" PRIu64 "\n", getpid(),
              context.cycle);
  flush_stdio();
  context.clone_requested = true;
  // The session opens the waveform on the next cycle, so this cycle's
  // falling edge is not recorded. -b/-e are ignored for that file.
  context.waveform_requested = true;
  return true;
}

int LightsssService::on_posedge(SimContext& context) {
  last_cycle_ = context.cycle;
  if (is_child_) {
    return child_cycle(context);
  }
  if (fail_enabled_ && context.cycle == fail_cycle_) {
    std::printf("LightSSS inject abort at cycle %" PRIu64 "\n", context.cycle);
    flush_stdio();
    return STATE_ABORT;
  }
  if (!enabled_ || share_ == nullptr) {
    return STATE_RUNNING;
  }
  uint64_t now = now_ms();
  if (have_forked_ && now - last_fork_ms_ < interval_ms_) {
    return STATE_RUNNING;
  }
  have_forked_ = true;
  last_fork_ms_ = now;
  if (!spawn(context)) {
    return STATE_ABORT;
  }
  return STATE_RUNNING;
}

void LightsssService::wakeup(uint64_t cycle) {
  if (share_ == nullptr || slots_.empty()) {
    return;
  }
  int oldest = slots_.back();
  share_->end_cycle.store(cycle, std::memory_order_relaxed);
  share_->oldest.store(oldest, std::memory_order_relaxed);
  for (int pid : slots_) {
    if (pid == oldest) {
      continue;
    }
    kill(pid, SIGKILL);
    wait_pid(pid);
  }
  std::printf("LightSSS wakeup oldest %d at cycle %" PRIu64 "\n", oldest,
              cycle);
  flush_stdio();
  share_->notgood.store(1, std::memory_order_relaxed);
  share_->flag.store(1, std::memory_order_release);
  wait_pid(oldest);
  slots_.clear();
}

void LightsssService::clear_children() {
  while (!slots_.empty()) {
    int pid = slots_.back();
    slots_.pop_back();
    kill(pid, SIGKILL);
    wait_pid(pid);
  }
}

void LightsssService::finish(int trap) {
  if (!enabled_ || is_child_ || share_ == nullptr) {
    return;
  }
  if (trap == STATE_ABORT) {
    wakeup(last_cycle_);
    return;
  }
  clear_children();
}
