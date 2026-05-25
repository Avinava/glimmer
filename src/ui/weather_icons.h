#pragma once
#include <stdint.h>

namespace WeatherIcon {
    void draw(int x, int y, uint8_t wmoCode, uint16_t color, int scale = 1);
}
