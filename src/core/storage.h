#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// All persisted settings. Loaded from /config.json on LittleFS, fall back to defaults.
struct Settings {
    String   wifiSSID;
    String   wifiPass;
    String   claudeKey;        // sk-ant-sid02-...
    String   codexToken;       // Bearer
    String   codexDeviceId;    // UUID
    uint32_t refreshMin    = 5;
    uint32_t channelSec    = 8;
    uint8_t  brightness    = 80;     // 0-100
    int8_t   tzOffset      = 0;      // legacy: hours from UTC, -12..+14
    int16_t  tzMinutes     = 0;      // signed offset from UTC in minutes;
                                     // takes precedence over tzOffset when non-zero.
                                     // Range: -720 (UTC-12:00) .. +840 (UTC+14:00).
                                     // Allows 30/45-minute timezones (India +330, Nepal +345).
    bool     showClaude    = true;
    bool     showCodex     = true;
    bool     showHome      = true;
    bool     showClock     = true;
    bool     showForecast  = true;
    bool     showAiDash    = true;
    bool     showInfo      = true;
    bool     autoRotate    = true;
    bool     claudeWeeklyHero = false;
    bool     codexWeeklyHero  = false;
    // Display polarity for this panel — keep true for SmallTV-Ultra ST7789.
    bool     invertDisplay = true;
    bool     nightDim      = false;
    uint8_t  nightStart    = 22;
    uint8_t  nightEnd      = 7;
    uint8_t  nightBright   = 15;

    // Weather (Open-Meteo, no key needed)
    float    weatherLat    = 0.0f;
    float    weatherLon    = 0.0f;
    bool     showWeather   = true;
    bool     useFahrenheit = false;

    // Personalization
    String   userName;                // shown on splash + birthday
    String   birthday;                // MM-DD format, e.g. "07-15"

    // Push API auth (also used for MCP)
    String   apiToken;                // bearer token for /push and /mcp
};

namespace Storage {
    void     begin();           // mounts LittleFS
    Settings load();
    bool     save(const Settings& s);
    void     factoryReset();
}
