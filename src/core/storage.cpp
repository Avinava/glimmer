#include "storage.h"
#include <LittleFS.h>
#include <time.h>

static const char* CFG_PATH = "/config.json";

void Storage::begin() {
    LittleFS.begin();
}

Settings Storage::load() {
    Settings s;
    File f = LittleFS.open(CFG_PATH, "r");
    if (!f) return s;
    JsonDocument doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) return s;
    s.wifiSSID      = doc["wifi_ssid"]      | "";
    s.wifiPass      = doc["wifi_pass"]      | "";
    s.claudeKey     = doc["claude_key"]     | "";
    s.codexToken    = doc["codex_token"]    | "";
    s.codexDeviceId    = doc["codex_dev"]      | "";
    s.codexModelLabel  = doc["codex_model"]    | "";
    s.refreshMin    = doc["refresh_min"]    | 5;
    s.channelSec    = doc["channel_sec"]    | 8;
    s.brightness    = doc["brightness"]     | 80;
    // Timezone: `tz_min` is authoritative. Configs written before it carried an
    // ambiguous pair (tz_minutes == 0 meant "use the hour-only tz_offset"), so
    // UTC could never be chosen; translate that once on load.
    if (!doc["tz_min"].isNull()) {
        s.tzMinutes = doc["tz_min"] | 0;
    } else {
        int16_t legacyMin = doc["tz_minutes"] | 0;
        int8_t  legacyHr  = doc["tz_offset"]  | 0;
        s.tzMinutes = legacyMin != 0 ? legacyMin : (int16_t)(legacyHr * 60);
    }
    s.showClaude    = doc["show_claude"]    | true;
    s.showCodex     = doc["show_codex"]     | true;
    s.showHome      = doc["show_home"]      | true;
    s.showClock     = doc["show_clock"]     | true;
    s.showForecast  = doc["show_forecast"]  | true;
    s.showAiDash    = doc["show_aidash"]    | true;
    s.showInfo      = doc["show_info"]      | true;
    s.showTrend     = doc["show_trend"]     | true;
    s.showStatus    = doc["show_status"]    | false;
    s.autoRotate    = doc["auto_rotate"]    | true;
    s.claudeWeeklyHero = doc["claude_weekly_hero"] | false;
    s.codexWeeklyHero  = doc["codex_weekly_hero"]  | false;
    s.clock24h      = doc["clock_24h"]      | true;
    s.invertDisplay = doc["invert_display"] | true;
    // Night: minute-precision window + mode. Older configs stored whole hours
    // and an on/off dim flag; translate those once on load.
    if (!doc["night_mode"].isNull()) {
        s.nightMode     = doc["night_mode"]      | 0;
        s.nightStartMin = doc["night_start_min"] | 22 * 60;
        s.nightEndMin   = doc["night_end_min"]   | 7 * 60;
    } else {
        s.nightMode     = (doc["night_dim"] | false) ? 1 : 0;
        s.nightStartMin = (uint16_t)((doc["night_start"] | 22) * 60);
        s.nightEndMin   = (uint16_t)((doc["night_end"]   | 7)  * 60);
    }
    s.nightBright   = doc["night_bright"]   | 15;
    s.weatherLat    = doc["weather_lat"]    | 0.0f;
    s.weatherLon    = doc["weather_lon"]    | 0.0f;
    s.showWeather   = doc["show_weather"]   | true;
    s.useFahrenheit = doc["fahrenheit"]     | false;
    s.userName      = doc["user_name"]      | "";
    s.birthday      = doc["birthday"]       | "";
    s.apiToken      = doc["api_token"]      | "";
    return s;
}

static bool writeAtomic(const std::function<size_t(File&)>& writer) {
    File f = LittleFS.open("/config.tmp", "w");
    if (!f) return false;
    size_t n = writer(f);
    f.close();
    if (n == 0) { LittleFS.remove("/config.tmp"); return false; }
    LittleFS.remove(CFG_PATH);
    return LittleFS.rename("/config.tmp", CFG_PATH);
}

bool Storage::save(const Settings& s) {
    JsonDocument doc;
    doc["wifi_ssid"]    = s.wifiSSID;
    doc["wifi_pass"]    = s.wifiPass;
    doc["claude_key"]   = s.claudeKey;
    doc["codex_token"]  = s.codexToken;
    doc["codex_dev"]    = s.codexDeviceId;
    doc["codex_model"]  = s.codexModelLabel;
    doc["refresh_min"]  = s.refreshMin;
    doc["channel_sec"]  = s.channelSec;
    doc["brightness"]   = s.brightness;
    doc["tz_min"]       = s.tzMinutes;
    doc["show_claude"]   = s.showClaude;
    doc["show_codex"]    = s.showCodex;
    doc["show_home"]     = s.showHome;
    doc["show_clock"]    = s.showClock;
    doc["show_forecast"] = s.showForecast;
    doc["show_aidash"]   = s.showAiDash;
    doc["show_info"]     = s.showInfo;
    doc["show_trend"]    = s.showTrend;
    doc["show_status"]   = s.showStatus;
    doc["auto_rotate"]   = s.autoRotate;
    doc["claude_weekly_hero"] = s.claudeWeeklyHero;
    doc["codex_weekly_hero"]  = s.codexWeeklyHero;
    doc["clock_24h"]     = s.clock24h;
    doc["invert_display"]= s.invertDisplay;
    doc["night_mode"]      = s.nightMode;
    doc["night_start_min"] = s.nightStartMin;
    doc["night_end_min"]   = s.nightEndMin;
    doc["night_bright"] = s.nightBright;
    doc["weather_lat"]  = s.weatherLat;
    doc["weather_lon"]  = s.weatherLon;
    doc["show_weather"] = s.showWeather;
    doc["fahrenheit"]   = s.useFahrenheit;
    doc["user_name"]    = s.userName;
    doc["birthday"]     = s.birthday;
    doc["api_token"]    = s.apiToken;
    return writeAtomic([&](File& f) { return serializeJson(doc, f); });
}

bool Storage::importRaw(const String& json) {
    JsonDocument d;
    if (deserializeJson(d, json) || !d.is<JsonObject>()) return false;
    return writeAtomic([&](File& f) { return f.print(json); });
}

void Storage::factoryReset() {
    LittleFS.remove(CFG_PATH);
}

void Storage::applyTimezone(const Settings& s) {
    // POSIX expresses the offset as "time to ADD to local to get UTC", so a
    // zone east of UTC is written with a NEGATIVE offset.
    int posixMin = -(int)s.tzMinutes;
    int posixH   = posixMin / 60;
    int posixM   = posixMin % 60; if (posixM < 0) posixM = -posixM;
    char tz[16];
    snprintf(tz, sizeof(tz), "UTC%+d:%02d", posixH, posixM);
    setenv("TZ", tz, 1);
    tzset();
}
