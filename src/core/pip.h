#pragma once
#include <Arduino.h>

// MoodId — historically used to drive the Pip mascot's facial expression.
// The mascot was removed in v0.13 (the design's "Mascot Speaks" principle was
// traded for information density on a desk display). The enum survives because
// Display::statusBar() still takes a MoodId for API stability — callers pass
// MoodId::NONE and the slot stays empty.
enum class MoodId : uint8_t {
    NONE,       // skip — leaves the slot empty (only value used in v0.13+)
    HAPPY,
    SLEEP,
    THINKING,
    EXCITED,
    FOCUS,
    WEATHER,
    LOADING,
    OFFLINE,
    HI,
    SIGNAL,
};
