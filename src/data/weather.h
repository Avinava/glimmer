#pragma once
#include <Arduino.h>
#include "storage.h"
#include "fetch_policy.h"

struct WeatherDay {
    float    tmin    = -999.0f;
    float    tmax    = -999.0f;
    uint8_t  code    = 0;
    float    uvMax   = -1.0f;
    time_t   sunrise = 0;
    time_t   sunset  = 0;
};

struct WeatherData {
    float       tempC      = -999.0f;
    float       feelsC     = -999.0f;
    uint8_t     code       = 0;       // WMO weather code
    bool        isDay      = true;
    int         humidity   = -1;
    float       windKmh    = -1.0f;
    WeatherDay  forecast[3];
    // Next 12 hours of precipitation probability (%, 255 = unknown),
    // rainStart = epoch of hourly slot 0.
    uint8_t     rainPct[12];
    time_t      rainStart  = 0;
    bool        valid      = false;
    time_t      lastOk     = 0;
    char        err[24]    = "";
    WeatherData() { memset(rainPct, 255, sizeof(rainPct)); }
};

namespace Weather {
    // The shared snapshot (Weather, Forecast and Home all read it).
    WeatherData& snapshot();
    const FetchPolicy::State& policy();

    // Restore the last good snapshot from /weather.json (shown as stale until
    // the first live fetch).
    void begin();

    // One fetch job; updates snapshot() + policy, persists on success.
    bool fetch(const Settings& s);

    // Minutes between successful fetches: the user's refresh interval, but
    // never more often than every 10 min (Open-Meteo updates ~15 min).
    uint32_t intervalMin(const Settings& s);
    bool     isStale(const Settings& s);
    bool     configured(const Settings& s);

    // "RAIN 70% 3PM" style hint when ≥ 40% chance in the next 12 h, else "".
    void rainHint(const Settings& s, char* buf, size_t n);

    // Convert WMO weather code (0..99) to a short text label.
    const char* describe(uint8_t code);

    // Helper for Fahrenheit/Celsius display.
    float       toDisplay(float c, bool fahrenheit);
}
