#pragma once
#include <Arduino.h>

// Pip — the SmallTV mascot. A 20×20 pixel CRT sprite with mood variations.
// Body is universal; only the inner face (rows 7..12, cols 4..11) changes.
//
// v0.8 ships 2 moods (THINKING, SIGNAL). v0.10 ships the full library.

enum class MoodId : uint8_t {
    NONE,       // skip — leaves the slot empty
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

namespace Pip {
    // Draw Pip at (x, y) with the given mood, scaled `scale`x (1 = 20 px, 2 = 40 px).
    // For NONE mood, this is a no-op.
    void draw(int x, int y, uint8_t scale, MoodId mood);
}
