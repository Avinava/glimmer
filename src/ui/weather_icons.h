#pragma once
#include <stdint.h>

namespace WeatherIcon {
    // Draw a 16x16 pixel-art weather icon for the given WMO code.
    // Uses direct-blit (same approach as Display::drawLogo).
    void draw(int x, int y, uint8_t wmoCode, uint16_t color);
}
