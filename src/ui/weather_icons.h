#pragma once
#include <stdint.h>

namespace WeatherIcon {
    // night: clear / mostly-clear codes (0, 1) draw a crescent moon instead of the sun.
    void draw(int x, int y, uint8_t wmoCode, uint16_t color, int scale = 1, bool night = false);
}
