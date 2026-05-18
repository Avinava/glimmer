#pragma once
#include <Arduino.h>
#include "storage.h"

struct WeatherDay {
    float    tmin = -999.0f;
    float    tmax = -999.0f;
    uint8_t  code = 0;
};

struct WeatherData {
    float       tempC      = -999.0f;
    float       feelsC     = -999.0f;
    uint8_t     code       = 0;       // WMO weather code
    int         humidity   = -1;
    float       windKmh    = -1.0f;
    WeatherDay  forecast[3];
    bool        valid      = false;
    char        err[24]    = "";
};

namespace Weather {
    bool        fetch(const Settings& s, WeatherData& out);

    // Convert WMO weather code (0..99) to a short text label and an icon glyph.
    const char* describe(uint8_t code);

    // Helper for Fahrenheit/Celsius display.
    float       toDisplay(float c, bool fahrenheit);
}
