#include "storage.h"
#include <LittleFS.h>

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
    s.codexDeviceId = doc["codex_dev"]      | "";
    s.refreshMin    = doc["refresh_min"]    | 5;
    s.channelSec    = doc["channel_sec"]    | 8;
    s.brightness    = doc["brightness"]     | 80;
    s.tzOffset      = doc["tz_offset"]      | 0;
    s.tzMinutes     = doc["tz_minutes"]     | 0;
    s.showClaude    = doc["show_claude"]    | true;
    s.showCodex     = doc["show_codex"]     | true;
    s.showHome      = doc["show_home"]      | true;
    s.showClock     = doc["show_clock"]     | true;
    s.showForecast  = doc["show_forecast"]  | true;
    s.showAiDash    = doc["show_aidash"]    | true;
    s.showInfo      = doc["show_info"]      | true;
    s.showTrends    = doc["show_trends"]    | true;
    s.autoRotate    = doc["auto_rotate"]    | true;
    s.invertDisplay = doc["invert_display"] | true;
    s.nightDim      = doc["night_dim"]      | false;
    s.nightStart    = doc["night_start"]    | 22;
    s.nightEnd      = doc["night_end"]      | 7;
    s.nightBright   = doc["night_bright"]   | 15;
    s.weatherLat    = doc["weather_lat"]    | 0.0f;
    s.weatherLon    = doc["weather_lon"]    | 0.0f;
    s.showWeather   = doc["show_weather"]   | true;
    s.useFahrenheit = doc["fahrenheit"]     | false;
    s.userName      = doc["user_name"]      | "";
    s.birthday      = doc["birthday"]       | "";
    s.timeOfDayMood = doc["mood"]           | true;
    s.autoBrightness= doc["auto_bright"]    | true;
    s.apiToken      = doc["api_token"]      | "";
    return s;
}

bool Storage::save(const Settings& s) {
    JsonDocument doc;
    doc["wifi_ssid"]    = s.wifiSSID;
    doc["wifi_pass"]    = s.wifiPass;
    doc["claude_key"]   = s.claudeKey;
    doc["codex_token"]  = s.codexToken;
    doc["codex_dev"]    = s.codexDeviceId;
    doc["refresh_min"]  = s.refreshMin;
    doc["channel_sec"]  = s.channelSec;
    doc["brightness"]   = s.brightness;
    doc["tz_offset"]    = s.tzOffset;
    doc["tz_minutes"]   = s.tzMinutes;
    doc["show_claude"]   = s.showClaude;
    doc["show_codex"]    = s.showCodex;
    doc["show_home"]     = s.showHome;
    doc["show_clock"]    = s.showClock;
    doc["show_forecast"] = s.showForecast;
    doc["show_aidash"]   = s.showAiDash;
    doc["show_info"]     = s.showInfo;
    doc["show_trends"]   = s.showTrends;
    doc["auto_rotate"]   = s.autoRotate;
    doc["invert_display"]= s.invertDisplay;
    doc["night_dim"]    = s.nightDim;
    doc["night_start"]  = s.nightStart;
    doc["night_end"]    = s.nightEnd;
    doc["night_bright"] = s.nightBright;
    doc["weather_lat"]  = s.weatherLat;
    doc["weather_lon"]  = s.weatherLon;
    doc["show_weather"] = s.showWeather;
    doc["fahrenheit"]   = s.useFahrenheit;
    doc["user_name"]    = s.userName;
    doc["birthday"]     = s.birthday;
    doc["mood"]         = s.timeOfDayMood;
    doc["auto_bright"]  = s.autoBrightness;
    doc["api_token"]    = s.apiToken;
    File f = LittleFS.open(CFG_PATH, "w");
    if (!f) return false;
    auto n = serializeJson(doc, f);
    f.close();
    return n > 0;
}

void Storage::factoryReset() {
    LittleFS.remove(CFG_PATH);
}
