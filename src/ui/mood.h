#pragma once
#include <Arduino.h>

namespace Mood {
    // Tint color for the given local hour (0..23). Shifts through the day:
    //   night     → cool dim blue
    //   morning   → warm orange
    //   work      → cool neutral
    //   evening   → warmer orange
    uint16_t tintFor(int hour);

    // Returns true if it's "alert hours" — when allowed to flash, animate, etc.
    bool isWorkingHours(int hour);

    // Auto-brightness suggestion (0..100) based on hour. Lower at night.
    int suggestedBrightness(int hour, int dayBright, bool dimAtNight,
                            int nightBright, int nightStart, int nightEnd);
}
