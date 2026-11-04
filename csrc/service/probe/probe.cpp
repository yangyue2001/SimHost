#include "service/probe/probe.h"

#include <cstdio>
#include <cstring>

ProbeService::ProbeService() : failed_(false), reason_("") {}

void ProbeService::fail(const char* reason) {
  if (!failed_) {
    failed_ = true;
    reason_ = reason;
  }
}

void ProbeService::push(const char* name, const uint64_t* words, int nwords) {
  if (failed_) {
    return;
  }
  if (name == nullptr || name[0] == '\0') {
    fail("probe name is missing");
    return;
  }
  if (std::strlen(name) >= kProbeNameLength) {
    fail("probe name is too long");
    return;
  }
  if (words == nullptr || nwords <= 0) {
    fail("probe sample is empty");
    return;
  }
  if (nwords > kProbeMaxWords) {
    fail("probe sample is wider than 64 words");
    return;
  }
  if (pending_.size() >= static_cast<size_t>(kProbeMaxPending)) {
    fail("probe queue is full");
    return;
  }

  ProbeSample sample{};
  std::strncpy(sample.name, name, kProbeNameLength - 1);
  sample.nwords = nwords;
  for (int index = 0; index < nwords; ++index) {
    sample.words[index] = words[index];
  }
  pending_.push_back(sample);
}

void ProbeService::clear() { pending_.clear(); }

void ProbeService::discard() {
  pending_.clear();
  failed_ = false;
  reason_ = "";
}
