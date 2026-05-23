// SmallTV web server.
//
// Static UI is served from LittleFS at /web/* (gzip-preferred). C++ side
// exposes a clean JSON API:
//   GET  /api/state    — live device state
//   GET  /api/settings — current settings (secrets masked as "***")
//   POST /api/settings — partial update; merges; optionally restarts
//   GET  /api/export   — raw config.json
//   POST /api/import   — replace config.json + restart
//   POST /api/factory-reset
//   POST /push         — ad-hoc card (existing)
//   POST /mcp          — JSON-RPC subset (existing)
//   POST /update       — OTA firmware + filesystem (ESP8266HTTPUpdateServer)

#include "web.h"
#include "display.h"
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

static ESP8266WebServer        server(80);
static ESP8266HTTPUpdateServer updater;
static Settings*               pSettings = nullptr;

// Defined in src/channels/ch_push.cpp
extern void pushCardSet(const char* title, const char* value, const char* subtitle,
                        uint16_t color, uint32_t durationMs);
extern void pushCardClear();

// ── Helpers ─────────────────────────────────────────────────────────────────

static uint16_t parseColor(const String& s) {
    String c = s; c.toLowerCase();
    if (c == "red"    || c == "coral" || c == "alert")  return 0xFA89;
    if (c == "amber"  || c == "warn")                   return 0xFE88;
    if (c == "green"  || c == "mint" || c == "ok")      return 0x8F2E;
    if (c == "blue"   || c == "sky"  || c == "info")    return 0x55BF;
    if (c == "lilac"  || c == "codex")                  return 0xC43F;
    return 0xFA89;
}

static bool checkAuth() {
    if (!pSettings || pSettings->apiToken.isEmpty()) return true;
    if (!server.hasHeader("Authorization")) return false;
    return server.header("Authorization") == ("Bearer " + pSettings->apiToken);
}

static const char* contentTypeFor(const String& path) {
    if (path.endsWith(".html") || path.endsWith(".html.gz")) return "text/html";
    if (path.endsWith(".css")  || path.endsWith(".css.gz"))  return "text/css";
    if (path.endsWith(".js")   || path.endsWith(".js.gz"))   return "application/javascript";
    if (path.endsWith(".json") || path.endsWith(".json.gz")) return "application/json";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".png"))  return "image/png";
    return "application/octet-stream";
}

// Serve any file under /web/, preferring its .gz variant if present.
// Returns true if the URL was handled.
static bool serveStatic(const String& uri) {
    String path = "/web";
    path += (uri == "/" ? String("/index.html") : uri);
    String gz = path + ".gz";

    bool useGz = LittleFS.exists(gz);
    String actual = useGz ? gz : path;
    if (!useGz && !LittleFS.exists(path)) return false;

    File f = LittleFS.open(actual, "r");
    if (!f) return false;
    // streamFile() auto-adds Content-Encoding: gzip when filename ends in .gz
    server.sendHeader("Cache-Control", "public, max-age=300");
    server.streamFile(f, contentTypeFor(path));
    f.close();
    return true;
}

// ── JSON API ────────────────────────────────────────────────────────────────

static String maskSecret(const String& s) {
    return s.isEmpty() ? String("") : String("***");
}

static void handleApiState() {
    JsonDocument d;
    d["fw"]                  = FW_VERSION;
    d["wifi"]                = WiFi.status() == WL_CONNECTED ? "connected" : "ap";
    d["ip"]                  = WiFi.status() == WL_CONNECTED
                                 ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    d["ssid"]                = WiFi.SSID();
    d["rssi"]                = WiFi.RSSI();
    d["uptime_s"]            = (uint32_t)(millis() / 1000);
    d["heap"]                = ESP.getFreeHeap();
    d["maxblk"]              = ESP.getMaxFreeBlockSize();
    d["cpu_mhz"]             = ESP.getCpuFreqMHz();
    d["claude_configured"]   = pSettings && !pSettings->claudeKey.isEmpty();
    d["codex_configured"]    = pSettings && !pSettings->codexToken.isEmpty();
    d["weather_configured"]  = pSettings && (pSettings->weatherLat != 0.0f || pSettings->weatherLon != 0.0f);
    String out; serializeJson(d, out);
    server.send(200, "application/json", out);
}

static void handleApiGetSettings() {
    if (!pSettings) { server.send(500, "application/json", "{\"error\":\"no settings\"}"); return; }
    Settings& s = *pSettings;
    JsonDocument d;
    d["wifiSSID"]      = s.wifiSSID;
    d["wifiPass"]      = maskSecret(s.wifiPass);
    d["claudeKey"]     = maskSecret(s.claudeKey);
    d["codexToken"]    = maskSecret(s.codexToken);
    d["codexDeviceId"] = s.codexDeviceId;
    d["apiToken"]      = maskSecret(s.apiToken);
    d["refreshMin"]    = s.refreshMin;
    d["channelSec"]    = s.channelSec;
    d["brightness"]    = s.brightness;
    d["tzOffset"]      = s.tzOffset;
    d["tzMinutes"]     = s.tzMinutes;
    d["showClaude"]    = s.showClaude;
    d["showCodex"]     = s.showCodex;
    d["showWeather"]   = s.showWeather;
    d["showHome"]      = s.showHome;
    d["showClock"]     = s.showClock;
    d["showForecast"]  = s.showForecast;
    d["showAiDash"]    = s.showAiDash;
    d["showInfo"]      = s.showInfo;
    d["autoRotate"]    = s.autoRotate;
    d["claudeWeeklyHero"] = s.claudeWeeklyHero;
    d["codexWeeklyHero"]  = s.codexWeeklyHero;
    d["invertDisplay"] = s.invertDisplay;
    d["nightDim"]      = s.nightDim;
    d["nightStart"]    = s.nightStart;
    d["nightEnd"]      = s.nightEnd;
    d["nightBright"]   = s.nightBright;
    d["weatherLat"]    = s.weatherLat;
    d["weatherLon"]    = s.weatherLon;
    d["useFahrenheit"] = s.useFahrenheit;
    d["userName"]      = s.userName;
    d["birthday"]      = s.birthday;
    String out; serializeJson(d, out);
    server.send(200, "application/json", out);
}

static void applyIfPresent(Settings& s, JsonDocument& d) {
    // Strings — only apply if key is present AND value is not "***" (the mask).
    auto applyStr = [&](const char* k, String& dst) {
        if (!d[k].is<const char*>()) return;
        const char* v = d[k].as<const char*>();
        if (v && strcmp(v, "***") != 0) dst = v;
    };
    applyStr("wifiSSID",      s.wifiSSID);
    applyStr("wifiPass",      s.wifiPass);
    applyStr("claudeKey",     s.claudeKey);
    applyStr("codexToken",    s.codexToken);
    applyStr("codexDeviceId", s.codexDeviceId);
    applyStr("apiToken",      s.apiToken);
    applyStr("userName",      s.userName);
    applyStr("birthday",      s.birthday);

    auto applyU32 = [&](const char* k, uint32_t& dst, uint32_t lo, uint32_t hi) {
        if (d[k].is<int>() || d[k].is<unsigned int>()) {
            int v = d[k].as<int>();
            if (v < (int)lo) v = lo; if (v > (int)hi) v = hi;
            dst = v;
        }
    };
    auto applyU8 = [&](const char* k, uint8_t& dst, int lo, int hi) {
        if (d[k].is<int>() || d[k].is<unsigned int>()) {
            int v = d[k].as<int>();
            if (v < lo) v = lo; if (v > hi) v = hi;
            dst = (uint8_t)v;
        }
    };
    auto applyI8 = [&](const char* k, int8_t& dst, int lo, int hi) {
        if (d[k].is<int>()) {
            int v = d[k].as<int>();
            if (v < lo) v = lo; if (v > hi) v = hi;
            dst = (int8_t)v;
        }
    };
    auto applyI16 = [&](const char* k, int16_t& dst, int lo, int hi) {
        if (d[k].is<int>()) {
            int v = d[k].as<int>();
            if (v < lo) v = lo; if (v > hi) v = hi;
            dst = (int16_t)v;
        }
    };
    auto applyBool = [&](const char* k, bool& dst) {
        if (d[k].is<bool>()) dst = d[k].as<bool>();
    };
    auto applyFloat = [&](const char* k, float& dst) {
        if (d[k].is<float>() || d[k].is<int>() || d[k].is<double>()) dst = d[k].as<float>();
    };

    applyU32("refreshMin",  s.refreshMin,  1, 60);
    applyU32("channelSec",  s.channelSec,  3, 60);
    applyU8 ("brightness",  s.brightness,  5, 100);
    applyI8 ("tzOffset",    s.tzOffset,   -12, 14);
    applyI16("tzMinutes",   s.tzMinutes,  -720, 840);
    applyU8 ("nightStart",  s.nightStart,  0, 23);
    applyU8 ("nightEnd",    s.nightEnd,    0, 23);
    applyU8 ("nightBright", s.nightBright, 1, 100);
    applyBool("showClaude",   s.showClaude);
    applyBool("showCodex",    s.showCodex);
    applyBool("showWeather",  s.showWeather);
    applyBool("showHome",     s.showHome);
    applyBool("showClock",    s.showClock);
    applyBool("showForecast", s.showForecast);
    applyBool("showAiDash",   s.showAiDash);
    applyBool("showInfo",     s.showInfo);
    applyBool("autoRotate",   s.autoRotate);
    applyBool("claudeWeeklyHero", s.claudeWeeklyHero);
    applyBool("codexWeeklyHero",  s.codexWeeklyHero);
    applyBool("invertDisplay",s.invertDisplay);
    applyBool("nightDim",    s.nightDim);
    applyBool("useFahrenheit", s.useFahrenheit);
    applyFloat("weatherLat", s.weatherLat);
    applyFloat("weatherLon", s.weatherLon);
}

static void handleApiPostSettings() {
    if (!pSettings) { server.send(500, "application/json", "{\"error\":\"no settings\"}"); return; }
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    applyIfPresent(*pSettings, d);
    Storage::save(*pSettings);

    // Apply runtime-mutable settings immediately so the user sees the effect.
    Display::setInvert(pSettings->invertDisplay);
    Display::setBrightness(pSettings->brightness);
    // POSIX TZ string: positive offset east of UTC must be expressed as a
    // NEGATIVE POSIX offset (POSIX expresses "time to ADD to local to get UTC").
    // tzMinutes wins if non-zero (supports +5:30 India / +5:45 Nepal / etc.);
    // otherwise fall back to hour-only tzOffset.
    int signedMin = (pSettings->tzMinutes != 0)
                  ? (int)pSettings->tzMinutes
                  : (int)pSettings->tzOffset * 60;
    int posixMin  = -signedMin;
    int posixH    = posixMin / 60;
    int posixM    = posixMin % 60; if (posixM < 0) posixM = -posixM;
    char tzBuf[16];
    snprintf(tzBuf, sizeof(tzBuf), "UTC%+d:%02d", posixH, posixM);
    setenv("TZ", tzBuf, 1);
    tzset();

    bool restart = d["_restart"] | false;
    server.send(200, "application/json", restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true}");
    if (restart) { delay(300); ESP.restart(); }
}

static void handleApiExport() {
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        // No saved config — synthesize from current
        handleApiGetSettings();
        return;
    }
    server.sendHeader("Content-Disposition", "attachment; filename=smalltv-config.json");
    server.streamFile(f, "application/json");
    f.close();
}

static void handleApiImport() {
    // Body is a full config.json. Validate it parses, then atomically replace
    // the file and restart.
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    File f = LittleFS.open("/config.json", "w");
    if (!f) { server.send(500, "application/json", "{\"error\":\"fs write\"}"); return; }
    f.print(server.arg("plain"));
    f.close();
    server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    delay(300);
    ESP.restart();
}

static void handleFactoryReset() {
    Storage::factoryReset();
    server.send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
}

static void handleReboot() {
    server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
    delay(300);
    ESP.restart();
}

// ── /push and /mcp (unchanged contract) ─────────────────────────────────────

static void handlePush() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    const char* title = doc["title"]    | "";
    const char* value = doc["value"]    | "";
    const char* sub   = doc["subtitle"] | "";
    String      col   = doc["color"]    | "coral";
    uint32_t    durS  = (uint32_t)(doc["duration_s"] | 30);
    if (durS > 300) durS = 300;
    pushCardSet(title, value, sub, parseColor(col), durS * 1000UL);
    server.send(200, "application/json", "{\"ok\":true}");
}

static void handleMcp() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument req;
    if (deserializeJson(req, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    const char* method = req["method"] | "";
    auto id            = req["id"];

    JsonDocument resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;

    if (strcmp(method, "initialize") == 0) {
        JsonObject r = resp["result"].to<JsonObject>();
        r["protocolVersion"] = "2024-11-05";
        JsonObject si = r["serverInfo"].to<JsonObject>();
        si["name"] = "smalltv"; si["version"] = FW_VERSION;
        r["capabilities"]["tools"] = JsonObject();
    }
    else if (strcmp(method, "tools/list") == 0) {
        JsonArray tools = resp["result"]["tools"].to<JsonArray>();
        auto add = [&](const char* name, const char* desc) {
            JsonObject t = tools.add<JsonObject>();
            t["name"] = name;
            t["description"] = desc;
            t["inputSchema"]["type"] = "object";
        };
        add("push_card",  "Flash a status card on the SmallTV screen for a short time");
        add("get_state",  "Return the device's current state (channel, usage, uptime)");
    }
    else if (strcmp(method, "tools/call") == 0) {
        const char* name = req["params"]["name"] | "";
        JsonObject args  = req["params"]["arguments"].as<JsonObject>();
        if (strcmp(name, "push_card") == 0) {
            const char* t = args["title"]    | "";
            const char* v = args["value"]    | "";
            const char* s = args["subtitle"] | "";
            String      c = args["color"]    | "coral";
            uint32_t   ds = (uint32_t)(args["duration_s"] | 30);
            if (ds > 300) ds = 300;
            pushCardSet(t, v, s, parseColor(c), ds * 1000UL);
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = "Card pushed";
        } else if (strcmp(name, "get_state") == 0) {
            JsonObject st = resp["result"]["content"][0]["json"].to<JsonObject>();
            st["fw"]       = FW_VERSION;
            st["uptime_s"] = (uint32_t)(millis() / 1000);
            st["heap"]     = ESP.getFreeHeap();
            st["maxblk"]   = ESP.getMaxFreeBlockSize();
            st["wifi"]     = WiFi.status() == WL_CONNECTED ? "connected" : "ap";
            resp["result"]["content"][0]["type"] = "json";
        } else {
            resp["error"]["code"]    = -32602;
            resp["error"]["message"] = "Unknown tool";
        }
    }
    else {
        resp["error"]["code"]    = -32601;
        resp["error"]["message"] = "Method not found";
    }

    String out; serializeJson(resp, out);
    server.send(200, "application/json", out);
}

// ── Public API ──────────────────────────────────────────────────────────────

void Web::begin(Settings& settings) {
    pSettings = &settings;
    updater.setup(&server);                                     // POST /update OTA
    // OTA progress UI — Update callback fires repeatedly during flash write.
    // We only repaint when integer-% changes (≤100 paints per flash, cheap).
    Update.onProgress([](size_t cur, size_t total) {
        if (!total) return;
        uint8_t pct = (uint8_t)((cur * 100UL) / total);
        Display::drawOtaProgress(pct);
    });

    // JSON API
    server.on("/api/state",          HTTP_GET,  handleApiState);
    server.on("/api/settings",       HTTP_GET,  handleApiGetSettings);
    server.on("/api/settings",       HTTP_POST, handleApiPostSettings);
    server.on("/api/export",         HTTP_GET,  handleApiExport);
    server.on("/api/import",         HTTP_POST, handleApiImport);
    server.on("/api/factory-reset",  HTTP_POST, handleFactoryReset);
    server.on("/api/reboot",         HTTP_POST, handleReboot);

    // Legacy + event endpoints
    server.on("/push",               HTTP_POST, handlePush);
    server.on("/mcp",                HTTP_POST, handleMcp);

    // Backwards-compat status (used by some scripts)
    server.on("/status",             HTTP_GET,  handleApiState);

    // Static file serving — fallback for everything else
    server.onNotFound([]() {
        if (!serveStatic(server.uri())) {
            server.send(404, "text/plain", "Not found");
        }
    });

    server.collectHeaders("Authorization");
    server.begin();
}

void Web::loop() {
    server.handleClient();
}
