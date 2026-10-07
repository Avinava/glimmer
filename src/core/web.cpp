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
//   POST /push         — attention card (see attention_json.h); GET lists, /push/clear removes
//   POST /hook         — trimmed agent hook event (tools/agents/glimmer-hook.sh)
//   POST /api/channel  — show a channel / next
//   POST /mcp          — JSON-RPC: push_card, clear_card, list_cards, get_state, show/next_channel
//   The API token (when set) guards /push*, /hook, /mcp, /api/refresh, /api/channel.
//   POST /update       — OTA firmware + filesystem (ESP8266HTTPUpdateServer)

#include "web.h"
#include "display.h"
#include "api.h"
#include "weather.h"
#include "vendor_status.h"
#include "attention.h"
#include "attention_json.h"
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

static ESP8266WebServer        server(80);
static ESP8266HTTPUpdateServer updater;
static Settings*               pSettings = nullptr;

// Defined in src/main.cpp
extern uint32_t mainApprovalTtlS();
extern bool     mainAgentDoneCards();

// Defined in src/main.cpp
extern const char* mainActiveChannelName();
extern int         mainEnabledCount();
extern int         mainTotalCount();
extern void        mainTriggerRefresh();
extern const char* mainEnabledChannelName(int idx);
extern const ClaudeData* mainClaudeData();
extern const CodexData*  mainCodexData();
extern bool        mainNightFace();
extern uint32_t    mainHeapRefusals();
extern uint32_t    mainMinMaxBlock();
extern uint32_t    mainNextFetchInMs();
extern bool        mainShowChannel(const char* name);
extern void        mainNextChannel();
extern void        mainSettingsChanged();

// ── Helpers ─────────────────────────────────────────────────────────────────

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

static void sourceJson(JsonObject o, const FetchPolicy::State& p, bool valid, const char* err) {
    o["valid"]     = valid;
    o["last_ok"]   = (uint32_t)p.lastOk;
    o["code"]      = p.lastCode;
    o["fails"]     = p.fails;
    o["auth_bad"]  = p.authLatched;
    o["blocked"]   = FetchPolicy::blocked(p);
    o["backoff_s"] = p.waitS;
    o["err"]       = err;
}

static void credJson(JsonObject o, Cred cred, time_t expiresAt, const ResetGrant& g) {
    o["cred"] = CredState::name(cred);
    if (expiresAt) o["expires_at"] = (uint32_t)expiresAt;
    if (g.left) {
        JsonObject r = o["resets"].to<JsonObject>();
        r["left"]    = g.left;
        r["usable"]  = g.usable;
        r["ends_at"] = (uint32_t)g.endsAt;
        r["title"]   = g.title;
    }
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
    d["min_maxblk"]          = mainMinMaxBlock();
    d["heap_refusals"]       = mainHeapRefusals();
    d["reset_reason"]        = ESP.getResetReason();
    d["build"]               = __DATE__ " " __TIME__;
    d["active_channel"]      = mainActiveChannelName();
    d["night_face"]          = mainNightFace();
    d["next_fetch_s"]        = mainNextFetchInMs() / 1000;
    d["claude_configured"]   = pSettings && !pSettings->claudeKey.isEmpty();
    d["codex_configured"]    = pSettings && !pSettings->codexToken.isEmpty();
    d["weather_configured"]  = pSettings && Weather::configured(*pSettings);
    // Diagnostics for the cold-boot Claude parse failure.
    d["claude_http"]         = Api::lastClaudeHttp();
    d["claude_parse"]        = Api::lastClaudeParse();
    if (const ClaudeData* cd = mainClaudeData()) {
        d["claude_err"]      = cd->err;
        d["claude_valid"]    = cd->valid;
    }
    // Per-source fetch health.
    JsonObject src = d["sources"].to<JsonObject>();
    const ClaudeData* cl = mainClaudeData();
    const CodexData*  cx = mainCodexData();
    JsonObject jcl = src["claude"].to<JsonObject>();
    JsonObject jcx = src["codex"].to<JsonObject>();
    sourceJson(jcl, Api::claudePolicy(), cl->valid, cl->err);
    sourceJson(jcx, Api::codexPolicy(),  cx->valid, cx->err);
    credJson(jcl, cl->cred, 0, cl->resets);
    credJson(jcx, cx->cred, cx->jwtExp, cx->resets);
    const WeatherData& w = Weather::snapshot();
    sourceJson(src["weather"].to<JsonObject>(), Weather::policy(), w.valid, w.err);
    if (pSettings && pSettings->showStatus) {
        JsonObject vs = d["vendor_status"].to<JsonObject>();
        const char* names[VendorStatus::COUNT] = {"claude", "openai"};
        for (int i = 0; i < VendorStatus::COUNT; i++) {
            const VendorStatus::Info& in = VendorStatus::get((VendorStatus::Vendor)i);
            JsonObject o = vs[names[i]].to<JsonObject>();
            o["level"]       = VendorStatus::levelName(in.level);
            o["description"] = in.description;
            o["last_ok"]     = (uint32_t)in.lastOk;
        }
    }
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
    d["codexDeviceId"]    = s.codexDeviceId;
    d["codexModelLabel"]  = s.codexModelLabel;
    d["apiToken"]      = maskSecret(s.apiToken);
    d["refreshMin"]    = s.refreshMin;
    d["channelSec"]    = s.channelSec;
    d["brightness"]    = s.brightness;
    d["tzMinutes"]     = s.tzMinutes;
    d["showClaude"]    = s.showClaude;
    d["showCodex"]     = s.showCodex;
    d["showWeather"]   = s.showWeather;
    d["showHome"]      = s.showHome;
    d["showClock"]     = s.showClock;
    d["showForecast"]  = s.showForecast;
    d["showAiDash"]    = s.showAiDash;
    d["showInfo"]      = s.showInfo;
    d["showTrend"]     = s.showTrend;
    d["showStatus"]    = s.showStatus;
    d["autoRotate"]    = s.autoRotate;
    d["claudeWeeklyHero"] = s.claudeWeeklyHero;
    d["codexWeeklyHero"]  = s.codexWeeklyHero;
    d["clock24h"]      = s.clock24h;
    d["invertDisplay"] = s.invertDisplay;
    d["nightMode"]     = s.nightMode;
    d["nightStartMin"] = s.nightStartMin;
    d["nightEndMin"]   = s.nightEndMin;
    d["nightBright"]   = s.nightBright;
    d["weatherLat"]    = s.weatherLat;
    d["weatherLon"]    = s.weatherLon;
    d["useFahrenheit"] = s.useFahrenheit;
    d["userName"]      = s.userName;
    d["pinApprovals"]   = s.pinApprovals;
    d["approvalTtlMin"] = s.approvalTtlMin;
    d["agentDoneCards"] = s.agentDoneCards;
    d["agentNightShow"] = s.agentNightShow;
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
    applyStr("codexDeviceId",    s.codexDeviceId);
    applyStr("codexModelLabel",  s.codexModelLabel);
    applyStr("apiToken",      s.apiToken);
    applyStr("userName",      s.userName);

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
    auto applyU16 = [&](const char* k, uint16_t& dst, int lo, int hi) {
        if (d[k].is<int>() || d[k].is<unsigned int>()) {
            int v = d[k].as<int>();
            if (v < lo) v = lo; if (v > hi) v = hi;
            dst = (uint16_t)v;
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
    applyI16("tzMinutes",   s.tzMinutes,  -720, 840);
    applyU8 ("nightMode",   s.nightMode,   0, 3);
    applyU16("nightStartMin", s.nightStartMin, 0, 1439);
    applyU16("nightEndMin",   s.nightEndMin,   0, 1439);
    applyU16("approvalTtlMin", s.approvalTtlMin, 5, 240);
    applyBool("pinApprovals",   s.pinApprovals);
    applyBool("agentDoneCards", s.agentDoneCards);
    applyBool("agentNightShow", s.agentNightShow);
    applyU8 ("nightBright", s.nightBright, 1, 100);
    applyBool("showClaude",   s.showClaude);
    applyBool("showCodex",    s.showCodex);
    applyBool("showWeather",  s.showWeather);
    applyBool("showHome",     s.showHome);
    applyBool("showClock",    s.showClock);
    applyBool("showForecast", s.showForecast);
    applyBool("showAiDash",   s.showAiDash);
    applyBool("showInfo",     s.showInfo);
    applyBool("showTrend",    s.showTrend);
    applyBool("showStatus",   s.showStatus);
    applyBool("autoRotate",   s.autoRotate);
    applyBool("claudeWeeklyHero", s.claudeWeeklyHero);
    applyBool("codexWeeklyHero",  s.codexWeeklyHero);
    applyBool("clock24h",     s.clock24h);
    applyBool("invertDisplay",s.invertDisplay);
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
    if (!Storage::save(*pSettings)) {
        server.send(507, "application/json", "{\"error\":\"save failed\"}");
        return;
    }

    bool restart = d["_restart"] | false;
    // Apply runtime-mutable settings immediately so the user sees the effect.
    if (!restart) mainSettingsChanged();
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
    server.sendHeader("Content-Disposition", "attachment; filename=glimmer-config.json");
    server.streamFile(f, "application/json");
    f.close();
}

static void handleApiImport() {
    // Body is a full config.json. Validate it parses, then atomically replace
    // the file (tmp + rename) and restart.
    if (!Storage::importRaw(server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json or fs write\"}");
        return;
    }
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

// Build an attention item from a /push body or MCP push_card arguments and
// queue it. Returns the item id written into idOut (for the reply).
static int pushFrom(JsonVariantConst d, char* idOut, size_t n) {
    static uint32_t s_seq = 0;
    Attention::Item it;
    Attention::fromJson(d, AttentionQueue::now(), mainApprovalTtlS(), ++s_seq, it);
    snprintf(idOut, n, "%s", it.id);
    return AttentionQueue::put(it);
}

static void sendJson(int code, JsonDocument& d) {
    String out; serializeJson(d, out);
    server.send(code, "application/json", out);
}

static void handlePush() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    char id[24];
    int slot = pushFrom(doc.as<JsonVariantConst>(), id, sizeof(id));
    JsonDocument r;
    r["ok"] = slot >= 0;
    r["id"] = id;
    if (slot < 0) r["error"] = "queue full of more important cards";
    sendJson(slot >= 0 ? 200 : 409, r);
}

// GET /push — the active queue, most important first.
static void handlePushList() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument r;
    JsonArray a = r["cards"].to<JsonArray>();
    const Attention::Queue& q = AttentionQueue::get();
    int ord[Attention::kMax], n = Attention::ordered(q, ord);
    uint32_t now = AttentionQueue::now();
    for (int i = 0; i < n; i++) Attention::toJson(q.items[ord[i]], now, a.add<JsonObject>());
    sendJson(200, r);
}

// POST /push/clear — {"id": "..."} | {"all": true}
static void handlePushClear() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    JsonDocument r;
    if (d["all"] | false) r["cleared"] = AttentionQueue::clearAll();
    else                  r["cleared"] = AttentionQueue::clear(d["id"] | "") ? 1 : 0;
    sendJson(200, r);
}

// POST /hook?agent=claude|codex — a TRIMMED agent hook event, sent by
// tools/agents/glimmer-hook.sh: {event, type, session, cwd, tool, detail,
// message}. Never post raw hook JSON here — a PostToolUse event carries the
// whole tool output, far beyond this device's RAM. Always answers 200 {} so
// a hook can never block or decide anything.
static void handleHook() {
    if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    JsonDocument d;
    if (server.arg("plain").length() > 2048 || deserializeJson(d, server.arg("plain"))) {
        server.send(200, "application/json", "{}");
        return;
    }
    Attention::HookEvent e;
    e.event   = d["event"]   | "";
    e.type    = d["type"]    | "";
    e.session = d["session"] | "";
    e.cwd     = d["cwd"]     | "";
    e.tool    = d["tool"]    | "";
    e.detail  = d["detail"]  | "";
    e.message = d["message"] | "";
    Attention::Agent agent = Attention::agentFrom(server.arg("agent").c_str());
    Attention::Item it, extra;
    switch (Attention::fromHook(agent, e, mainAgentDoneCards(), mainApprovalTtlS(),
                                AttentionQueue::now(), it, &extra)) {
        case Attention::HOOK_UPSERT:
            if (extra.used) AttentionQueue::put(extra);
            AttentionQueue::put(it);
            break;
        case Attention::HOOK_CLEAR:  AttentionQueue::clear(it.id); break;
        default: break;
    }
    server.send(200, "application/json", "{}");
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

        // push_card
        JsonObject t1 = tools.add<JsonObject>();
        t1["name"] = "push_card";
        t1["description"] = F(
            "Show a card on the user's glimmer desk display. Use kind=approval when you are "
            "blocked waiting for the user to approve something, kind=input when you need an "
            "answer, progress for long work (re-send the same id to update it), success/error "
            "when a task finishes. Keep it short; never include secrets or file contents.");
        JsonObject s1 = t1["inputSchema"].to<JsonObject>();
        s1["type"] = "object";
        JsonObject p1 = s1["properties"].to<JsonObject>();
        auto prop = [&](const char* k, const char* type, const __FlashStringHelper* desc) {
            p1[k]["type"] = type; p1[k]["description"] = desc;
        };
        prop("title",    "string",  F("Headline, <= 27 chars, e.g. 'TESTS PASSED'"));
        prop("value",    "string",  F("Optional big value, <= 15 chars, e.g. '142/142'"));
        prop("body",     "string",  F("Optional detail line, <= 39 chars"));
        prop("kind",     "string",  F("approval | input | error | warning | success | progress | info"));
        prop("id",       "string",  F("Stable id: re-send it to update the card in place, or to clear it"));
        prop("agent",    "string",  F("claude | codex | other (tag on the card)"));
        prop("project",  "string",  F("Repo / project name, <= 15 chars"));
        prop("progress", "integer", F("0-100, draws a progress bar"));
        prop("ttl_s",    "integer", F("Seconds until it disappears; 0 = until cleared (max 86400)"));
        prop("display",  "string",  F("interrupt (default: takes the screen briefly) | queue (only listed)"));
        prop("urgent",   "boolean", F("May wake a dark screen at night. Use sparingly."));
        s1["required"].to<JsonArray>().add("title");

        // clear_card
        JsonObject tc = tools.add<JsonObject>();
        tc["name"] = "clear_card";
        tc["description"] = F("Remove a card you pushed (by id), or all cards with all=true.");
        JsonObject sc = tc["inputSchema"].to<JsonObject>();
        sc["type"] = "object";
        sc["properties"]["id"]["type"]  = "string";
        sc["properties"]["all"]["type"] = "boolean";

        // list_cards
        JsonObject tl = tools.add<JsonObject>();
        tl["name"] = "list_cards";
        tl["description"] = F("List the cards currently on the display, most important first.");
        tl["inputSchema"]["type"] = "object";

        // get_state
        JsonObject t2 = tools.add<JsonObject>();
        t2["name"] = "get_state";
        t2["description"] = F("Return the device's current state (active channel, usage, uptime, heap, wifi)");
        t2["inputSchema"]["type"] = "object";

        // show_channel
        JsonObject t3 = tools.add<JsonObject>();
        t3["name"] = "show_channel";
        t3["description"] = F("Switch the screen to a channel by name (see enabled_channels in get_state)");
        JsonObject s3 = t3["inputSchema"].to<JsonObject>();
        s3["type"] = "object";
        s3["properties"]["name"]["type"] = "string";
        s3["properties"]["name"]["description"] = F("Channel name, e.g. Claude, Codex, Weather, Trend");
        s3["required"].to<JsonArray>().add("name");

        // next_channel
        JsonObject t4 = tools.add<JsonObject>();
        t4["name"] = "next_channel";
        t4["description"] = F("Advance the screen to the next enabled channel");
        t4["inputSchema"]["type"] = "object";
    }
    else if (strcmp(method, "tools/call") == 0) {
        const char* name = req["params"]["name"] | "";
        JsonObject args  = req["params"]["arguments"].as<JsonObject>();
        if (strcmp(name, "push_card") == 0) {
            char id[24];
            int slot = pushFrom(args, id, sizeof(id));
            char msg[64];
            if (slot >= 0) snprintf(msg, sizeof(msg), "Card shown (id %s)", id);
            else           snprintf(msg, sizeof(msg), "Not shown: queue full of more important cards");
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = msg;
            if (slot < 0) resp["result"]["isError"] = true;
        } else if (strcmp(name, "clear_card") == 0) {
            int n = (args["all"] | false) ? AttentionQueue::clearAll()
                                          : (AttentionQueue::clear(args["id"] | "") ? 1 : 0);
            char msg[32]; snprintf(msg, sizeof(msg), "Cleared %d", n);
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = msg;
        } else if (strcmp(name, "list_cards") == 0) {
            JsonDocument list;
            JsonArray a = list.to<JsonArray>();
            const Attention::Queue& q = AttentionQueue::get();
            int ord[Attention::kMax], n = Attention::ordered(q, ord);
            uint32_t now = AttentionQueue::now();
            for (int i = 0; i < n; i++) Attention::toJson(q.items[ord[i]], now, a.add<JsonObject>());
            String txt; serializeJson(list, txt);
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = txt;
        } else if (strcmp(name, "get_state") == 0) {
            JsonObject st = resp["result"]["content"][0]["json"].to<JsonObject>();
            st["fw"]              = FW_VERSION;
            st["uptime_s"]        = (uint32_t)(millis() / 1000);
            st["heap"]            = ESP.getFreeHeap();
            st["maxblk"]          = ESP.getMaxFreeBlockSize();
            st["wifi"]            = WiFi.status() == WL_CONNECTED ? "connected" : "ap";
            st["rssi"]            = WiFi.RSSI();
            st["active_channel"]  = mainActiveChannelName();
            st["total_channels"]  = mainTotalCount();
            st["brightness"]      = pSettings->brightness;
            JsonArray ec = st["enabled_channels"].to<JsonArray>();
            for (int i = 0; i < mainEnabledCount(); i++) {
                const char* n = mainEnabledChannelName(i);
                if (n) ec.add(n);
            }
            const ClaudeData* cd = mainClaudeData();
            if (cd && cd->valid) {
                JsonArray ma = st["claude_models"].to<JsonArray>();
                for (int i = 0; i < 3; i++) {
                    if (cd->models[i].label[0]) {
                        JsonObject m = ma.add<JsonObject>();
                        m["label"] = cd->models[i].label;
                        m["pct"]   = (int)cd->models[i].pct;
                    }
                }
            }
            resp["result"]["content"][0]["type"] = "json";
        } else if (strcmp(name, "show_channel") == 0) {
            const char* ch = args["name"] | "";
            bool ok = mainShowChannel(ch);
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = ok ? "Switched" : "No enabled channel with that name";
            if (!ok) resp["result"]["isError"] = true;
        } else if (strcmp(name, "next_channel") == 0) {
            mainNextChannel();
            resp["result"]["content"][0]["type"] = "text";
            resp["result"]["content"][0]["text"] = mainActiveChannelName();
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
    server.on("/api/refresh",        HTTP_POST, []() {
        if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
        mainTriggerRefresh();
        server.send(200, "application/json", "{\"ok\":true}");
    });

    // Manual channel switching: {"name":"Claude"} or {"action":"next"}
    server.on("/api/channel",        HTTP_POST, []() {
        if (!checkAuth()) { server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
        JsonDocument d;
        if (deserializeJson(d, server.arg("plain"))) {
            server.send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        if (strcmp(d["action"] | "", "next") == 0) mainNextChannel();
        else if (!mainShowChannel(d["name"] | "")) {
            server.send(404, "application/json", "{\"error\":\"no enabled channel with that name\"}");
            return;
        }
        String out = String("{\"ok\":true,\"active\":\"") + mainActiveChannelName() + "\"}";
        server.send(200, "application/json", out);
    });

    // Legacy + event endpoints
    server.on("/push",               HTTP_POST, handlePush);
    server.on("/push",               HTTP_GET,  handlePushList);
    server.on("/push/clear",         HTTP_POST, handlePushClear);
    server.on("/hook",               HTTP_POST, handleHook);
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
