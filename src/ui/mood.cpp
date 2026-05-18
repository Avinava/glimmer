#include "mood.h"
#include "theme.h"

namespace Mood {

// 16-bit RGB565 colors that read well as a 2-3 pixel border on top of the BG.
//   night    — cool dim blue/purple
//   dawn     — gentle warm orange (rosy)
//   morning  — warmer orange
//   day      — soft neutral cyan
//   afternoon— soft amber
//   evening  — saturated warm orange
//   dusk     — deep purple
static uint16_t lookup(int hour) {
    switch (hour) {
        case 0: case 1: case 2: case 3: case 4: case 5: return 0x1186;  // deep indigo
        case 6:                                          return 0x71CC;  // dawn rose
        case 7: case 8:                                  return 0xFB04;  // morning orange
        case 9: case 10:                                 return 0xCEDB;  // soft light blue
        case 11: case 12: case 13: case 14: case 15:     return 0xDEFB;  // bright neutral
        case 16: case 17:                                return 0xFD46;  // late afternoon amber
        case 18: case 19:                                return 0xFB04;  // evening orange
        case 20: case 21:                                return 0x800F;  // dusk purple
        default:                                         return 0x1186;
    }
}

uint16_t tintFor(int hour) {
    if (hour < 0 || hour > 23) hour = 12;
    return lookup(hour);
}

bool isWorkingHours(int hour) {
    return hour >= 8 && hour < 20;
}

int suggestedBrightness(int hour, int dayBright, bool dimAtNight,
                        int nightBright, int nightStart, int nightEnd) {
    if (!dimAtNight) return dayBright;
    bool inNight = (nightStart <= nightEnd)
                   ? (hour >= nightStart && hour < nightEnd)
                   : (hour >= nightStart || hour < nightEnd);
    return inNight ? nightBright : dayBright;
}

}  // namespace Mood
