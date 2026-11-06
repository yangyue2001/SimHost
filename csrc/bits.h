#ifndef SIM_BITS_H
#define SIM_BITS_H

#include <cstdint>

// Inclusive. bits<60, 63>(satp) is satp[63:60].
template <int Lo, int Hi>
constexpr uint64_t bits(uint64_t value) {
  static_assert(Lo >= 0 && Hi >= Lo && Hi < 64, "bit range is outside 0..63");
  constexpr int width = Hi - Lo + 1;
  if constexpr (width == 64) {
    return value;
  } else {
    return (value >> Lo) & ((uint64_t{1} << width) - 1);
  }
}

#endif
