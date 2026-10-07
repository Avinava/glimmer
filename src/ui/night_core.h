#pragma once
// Night window + mode logic. Pure (no Arduino) so it is host-tested.
#include <stdint.h>

namespace Night {

enum Mode : uint8_t {
    NONE  = 0,   // no night behaviour
    DIM   = 1,   // normal rotation at night brightness
    CLOCK = 2,   // dim night face only (time + date), no rotation
    DARK  = 3,   // backlight off; an alert push card wakes it
};

// Minutes past local midnight. The window wraps past midnight; start == end
// disables it.
inline bool inWindow(int minuteOfDay, int startMin, int endMin) {
    if (startMin == endMin) return false;
    if (startMin < endMin) return minuteOfDay >= startMin && minuteOfDay < endMin;
    return minuteOfDay >= startMin || minuteOfDay < endMin;
}

// Effective backlight % for the moment. A DARK night is 0 unless a push card
// is waking the panel, in which case it uses the night level.
inline int brightness(Mode mode, bool night, bool pushWake, int dayPct, int nightPct) {
    if (!night || mode == NONE) return dayPct;
    if (mode == DARK) return pushWake ? nightPct : 0;
    return nightPct;
}

}  // namespace Night
