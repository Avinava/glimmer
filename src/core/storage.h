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
    String   codexModelLabel;  // user-set status bar label, e.g. "GPT-5"
    uint32_t refreshMin    = 5;
    uint32_t channelSec    = 8;
    uint8_t  brightness    = 80;     // 0-100
    int16_t  tzMinutes     = 0;      // signed offset from UTC in minutes, -720..+840.
                                     // Allows 30/45-minute timezones (India +330, Nepal +345).
    bool     showClaude    = true;
    bool     showCodex     = true;
    bool     showHome      = true;
    bool     showClock     = true;
    bool     showForecast  = true;
    bool     showAiDash    = true;
    bool     showInfo      = true;
    bool     showTrend     = true;
    bool     showStatus    = false;  // vendor status pages (2 extra TLS calls / 15 min)
    bool     autoRotate    = true;
    bool     claudeWeeklyHero = false;
    bool     codexWeeklyHero  = false;
    bool     clock24h      = true;
    // Display polarity for this panel — keep true for SmallTV-Ultra ST7789.
    bool     invertDisplay = true;

    // Night window (local minutes past midnight; start == end disables).
    uint8_t  nightMode     = 0;      // Night::Mode — 0 none, 1 dim, 2 clock, 3 dark
    uint16_t nightStartMin = 22 * 60;
    uint16_t nightEndMin   = 7 * 60;
    uint8_t  nightBright   = 15;

    // Weather (Open-Meteo, no key needed)
    float    weatherLat    = 0.0f;
    float    weatherLon    = 0.0f;
    bool     showWeather   = true;
    bool     useFahrenheit = false;

    // Personalization
    String   userName;                // shown on the greeting splash + clock

    // Push API auth (also used for MCP)
    String   apiToken;                // bearer token for /push and /mcp
};

namespace Storage {
    void     begin();           // mounts LittleFS
    Settings load();
    bool     save(const Settings& s);
    // Validate a full config.json body and atomically replace the stored one.
    bool     importRaw(const String& json);
    void     factoryReset();

    // POSIX TZ string for the fixed offset ("UTC-5:30" for +05:30) and apply it.
    void     applyTimezone(const Settings& s);
}
