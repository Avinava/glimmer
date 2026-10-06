// glimmer — main orchestrator.
//
// Architecture:
//   core/    hardware + I/O (display, wifi, time, storage, web, OTA)
//   data/    external API clients (claude, codex, weather, status) + history
//   channels/ self-contained renderers, registered in kChannels[]
//
// Adding a channel = drop one .cpp into src/channels/ + add a row to kChannels[].
//
// Fetching is a staggered job scheduler, not a blocking "refresh all": each
// loop pass starts at most one job, jobs are spaced ≥ 3 s apart, and a TLS job
// only starts when the largest free heap block clears Api::kTlsFloor. The web
// server, channel ticks and Wi-Fi checks keep running between jobs.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <time.h>

#include "config.h"
#include "theme.h"
#include "display.h"
#include "storage.h"
#include "web.h"
#include "api.h"
#include "weather.h"
#include "history.h"
#include "vendor_status.h"
#include "channel.h"
#include "night_core.h"

// ── Channel registry — declared in their own .cpp files ─────────────────────
extern bool chClaudeEnabled(const ChannelCtx&);  extern void chClaudeDraw(const ChannelCtx&);
extern bool chCodexEnabled (const ChannelCtx&);  extern void chCodexDraw (const ChannelCtx&);
extern bool chClockEnabled (const ChannelCtx&);  extern void chClockDraw (const ChannelCtx&);
extern bool chInfoEnabled  (const ChannelCtx&);  extern void chInfoDraw  (const ChannelCtx&);
extern bool chWeatherEnabled (const ChannelCtx&); extern void chWeatherDraw (const ChannelCtx&);
extern bool chPushEnabled    (const ChannelCtx&); extern void chPushDraw    (const ChannelCtx&);
extern bool chHomeEnabled    (const ChannelCtx&); extern void chHomeDraw    (const ChannelCtx&);
extern bool chAiDashEnabled  (const ChannelCtx&); extern void chAiDashDraw  (const ChannelCtx&);
extern bool chForecastEnabled(const ChannelCtx&); extern void chForecastDraw(const ChannelCtx&);
extern bool chTrendEnabled   (const ChannelCtx&); extern void chTrendDraw   (const ChannelCtx&);
extern bool chNightEnabled   (const ChannelCtx&); extern void chNightDraw   (const ChannelCtx&);
extern void chPushTick       (const ChannelCtx&);
extern void chClockTick      (const ChannelCtx&);
extern void chHomeTick       (const ChannelCtx&);
extern void chClaudeTick     (const ChannelCtx&);
extern void chCodexTick      (const ChannelCtx&);
extern void chAiDashTick     (const ChannelCtx&);
extern void chWeatherTick    (const ChannelCtx&);
extern void chForecastTick   (const ChannelCtx&);
extern void chTrendTick      (const ChannelCtx&);
extern void chInfoTick       (const ChannelCtx&);
extern void chNightTick      (const ChannelCtx&);

static const Channel kChannels[] = {
    //  name        enabled              draw                  tick
    { "Push",     chPushEnabled,     chPushDraw,     chPushTick     },
    { "Night",    chNightEnabled,    chNightDraw,    chNightTick    },
    { "Home",     chHomeEnabled,     chHomeDraw,     chHomeTick     },
    { "Claude",   chClaudeEnabled,   chClaudeDraw,   chClaudeTick   },
    { "Codex",    chCodexEnabled,    chCodexDraw,    chCodexTick    },
    { "AI",       chAiDashEnabled,   chAiDashDraw,   chAiDashTick   },
    { "Trend",    chTrendEnabled,    chTrendDraw,    chTrendTick    },
    { "Weather",  chWeatherEnabled,  chWeatherDraw,  chWeatherTick  },
    { "Forecast", chForecastEnabled, chForecastDraw, chForecastTick },
    { "Clock",    chClockEnabled,    chClockDraw,    chClockTick    },
    { "Info",     chInfoEnabled,     chInfoDraw,     chInfoTick     },
};
static constexpr int kChannelCount = sizeof(kChannels) / sizeof(kChannels[0]);

// ── Globals ──────────────────────────────────────────────────────────────────

static Settings    g_settings;
static bool        g_apMode      = false;
static ClaudeData  g_claude;
static CodexData   g_codex;

static int         g_activeIdx[kChannelCount];   // indices into kChannels[] that are enabled
static int         g_activeCount = 0;
static int         g_activePtr   = 0;     // which active channel is on screen

static uint32_t    g_lastRefresh = 0;     // millis of the last completed fetch job
static uint32_t    g_lastSlide   = 0;
static bool        g_nightFace   = false; // night window + mode clock/dark

// ── Fetch scheduler state ────────────────────────────────────────────────────

enum Job : uint8_t { JOB_CLAUDE, JOB_CODEX, JOB_WEATHER, JOB_STATUS_CLAUDE, JOB_STATUS_OPENAI, JOB_COUNT };
static const char* kJobName[JOB_COUNT] = {"claude", "codex", "weather", "status-claude", "status-openai"};

static uint32_t g_jobDue[JOB_COUNT];      // millis when due (0 = now)
static uint32_t g_lastJobStart = 0;
static uint32_t g_heapRefusals = 0;       // jobs deferred by the heap gate
static uint32_t g_minMaxBlk    = 0xFFFFFFFF;

// ── Accessors for web.cpp / channels ────────────────────────────────────────

const char* mainActiveChannelName() {
    if (g_activeCount == 0) return "none";
    return kChannels[g_activeIdx[g_activePtr]].name;
}
int  mainEnabledCount()    { return g_activeCount; }
int  mainTotalCount()      { return kChannelCount; }
uint32_t mainLastRefreshMs() { return g_lastRefresh; }
const char* mainEnabledChannelName(int idx) {
    if (idx < 0 || idx >= g_activeCount) return nullptr;
    return kChannels[g_activeIdx[idx]].name;
}
const ClaudeData* mainClaudeData() { return &g_claude; }
const CodexData*  mainCodexData()  { return &g_codex; }
bool     mainNightFace()    { return g_nightFace; }
uint32_t mainHeapRefusals() { return g_heapRefusals; }
uint32_t mainMinMaxBlock()  { return g_minMaxBlk; }

// ── Helpers ──────────────────────────────────────────────────────────────────

static ChannelCtx makeCtx() {
    return ChannelCtx { &g_settings, &g_claude, &g_codex, millis() };
}

static bool timeSynced() { return time(nullptr) > 1000000000L; }

static bool nightNow() {
    if (!timeSynced() || g_settings.nightMode == Night::NONE) return false;
    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    return Night::inWindow(tm.tm_hour * 60 + tm.tm_min,
                           g_settings.nightStartMin, g_settings.nightEndMin);
}

static void applyBrightness() {
    ChannelCtx ctx = makeCtx();
    bool night = nightNow();
    Display::setBrightness(Night::brightness((Night::Mode)g_settings.nightMode, night,
                                             chPushEnabled(ctx), g_settings.brightness,
                                             g_settings.nightBright));
}

static void recomputeActive() {
    // Keep showing the same channel across a recompute when it is still enabled.
    const char* cur = g_activeCount ? kChannels[g_activeIdx[g_activePtr]].name : nullptr;
    g_nightFace = nightNow() && g_settings.nightMode >= Night::CLOCK;
    ChannelCtx ctx = makeCtx();
    g_activeCount = 0;
    for (int i = 0; i < kChannelCount; i++) {
        // The night face replaces rotation: only it (and an alert card) runs.
        if (g_nightFace && strcmp(kChannels[i].name, "Night") && strcmp(kChannels[i].name, "Push"))
            continue;
        if (kChannels[i].enabled(ctx)) g_activeIdx[g_activeCount++] = i;
    }
    g_activePtr = 0;
    for (int i = 0; cur && i < g_activeCount; i++)
        if (!strcmp(kChannels[g_activeIdx[i]].name, cur)) { g_activePtr = i; break; }
}

static void drawActive() {
    if (g_activeCount == 0) {
        Display::drawError("No channels", "Configure web UI");
        return;
    }
    // Re-arm splash/connecting/OTA partial-redraw state in case we re-enter
    // a system screen later (e.g., WiFi reconnect drops to drawConnecting).
    Display::resetSystemScreens();
    // Quiet cut: the channel's draw() clears + paints in ~80 ms.
    ChannelCtx ctx = makeCtx();
    kChannels[g_activeIdx[g_activePtr]].draw(ctx);
}

// Recompute, and repaint only when what should be on screen changed.
static void refreshScreen(bool force) {
    const char* before = g_activeCount ? kChannels[g_activeIdx[g_activePtr]].name : nullptr;
    int countBefore = g_activeCount;
    recomputeActive();
    const char* after = g_activeCount ? kChannels[g_activeIdx[g_activePtr]].name : nullptr;
    if (force || before != after || countBefore != g_activeCount) {
        drawActive();
        g_lastSlide = millis();
    }
}

// 2-px progress strip at y=230. Fills in current channel's theme color as
// the slide window elapses. Hidden while a Push card or the night face is up,
// and left empty when auto-rotate is off (nothing is counting down).
static void drawIndicator(uint32_t now) {
    using namespace Layout;
    if (g_apMode || g_activeCount <= 1 || g_nightFace) return;
    const char* name = kChannels[g_activeIdx[g_activePtr]].name;
    if (!strcmp(name, "Push")) return;

    int w = 0;
    if (g_settings.autoRotate) {
        uint32_t slideMs = (uint32_t)g_settings.channelSec * 1000UL;
        uint32_t elapsed = now - g_lastSlide;
        if (elapsed > slideMs) elapsed = slideMs;
        w = (int)((uint64_t)SCREEN_W * elapsed / slideMs);
    }
    tft.fillRect(0, INDICATOR_Y, SCREEN_W, INDICATOR_H, Theme::PANEL);
    if (w > 0) tft.fillRect(0, INDICATOR_Y, w, INDICATOR_H, Theme::channelColor(name));
}

// Manual switching (web /api/channel, MCP).
bool mainShowChannel(const char* name) {
    recomputeActive();
    for (int i = 0; i < g_activeCount; i++) {
        if (!strcasecmp(kChannels[g_activeIdx[i]].name, name)) {
            g_activePtr = i;
            drawActive();
            g_lastSlide = millis();
            return true;
        }
    }
    return false;
}

void mainNextChannel() {
    recomputeActive();
    if (g_activeCount == 0) return;
    g_activePtr = (g_activePtr + 1) % g_activeCount;
    drawActive();
    g_lastSlide = millis();
}

// ── Fetch scheduler ──────────────────────────────────────────────────────────

static bool jobEnabled(Job j) {
    switch (j) {
        case JOB_CLAUDE:  return !g_settings.claudeKey.isEmpty();
        case JOB_CODEX:   return !g_settings.codexToken.isEmpty();
        // Weather feeds the Weather, Forecast and Home channels.
        case JOB_WEATHER: return Weather::configured(g_settings)
                              && (g_settings.showWeather || g_settings.showHome || g_settings.showForecast);
        case JOB_STATUS_CLAUDE: return g_settings.showStatus && !g_settings.claudeKey.isEmpty();
        case JOB_STATUS_OPENAI: return g_settings.showStatus && !g_settings.codexToken.isEmpty();
        default: return false;
    }
}

static uint32_t jobIntervalMs(Job j) {
    switch (j) {
        case JOB_WEATHER:       return Weather::intervalMin(g_settings) * 60000UL;
        case JOB_STATUS_CLAUDE:
        case JOB_STATUS_OPENAI: return VendorStatus::kIntervalMin * 60000UL;
        default:                return g_settings.refreshMin * 60000UL;
    }
}

static const FetchPolicy::State& jobPolicy(Job j) {
    switch (j) {
        case JOB_CLAUDE:        return Api::claudePolicy();
        case JOB_CODEX:         return Api::codexPolicy();
        case JOB_WEATHER:       return Weather::policy();
        case JOB_STATUS_CLAUDE: return VendorStatus::policy(VendorStatus::CLAUDE);
        default:                return VendorStatus::policy(VendorStatus::OPENAI);
    }
}

void mainTriggerRefresh() {
    uint32_t now = millis();
    for (auto& d : g_jobDue) d = now;
}

uint32_t mainNextFetchInMs() {
    uint32_t now = millis(), best = 0xFFFFFFFF;
    for (int j = 0; j < JOB_COUNT; j++) {
        if (!jobEnabled((Job)j)) continue;
        int32_t in = (int32_t)(g_jobDue[j] - now);
        uint32_t v = in > 0 ? (uint32_t)in : 0;
        if (v < best) best = v;
    }
    return best == 0xFFFFFFFF ? 0 : best;
}

// What a channel's draw() depends on, coarsely: a change means a full redraw
// (loading → data → error transitions), otherwise tick() picks up new values.
static uint8_t usageShape(bool valid, const char* err, bool authErr) {
    return (valid ? 1 : 0) | (err[0] ? 2 : 0) | (authErr ? 4 : 0);
}

static void runJob(Job j) {
    uint8_t before = 0, after = 0;
    bool ok = false;
    switch (j) {
        case JOB_CLAUDE:
            before = usageShape(g_claude.valid, g_claude.err, g_claude.authErr);
            ok = Api::fetchClaude(g_settings, g_claude);
            after = usageShape(g_claude.valid, g_claude.err, g_claude.authErr);
            break;
        case JOB_CODEX:
            before = usageShape(g_codex.valid, g_codex.err, g_codex.authErr);
            ok = Api::fetchCodex(g_settings, g_codex);
            after = usageShape(g_codex.valid, g_codex.err, g_codex.authErr);
            break;
        case JOB_WEATHER: {
            const WeatherData& w = Weather::snapshot();
            before = usageShape(w.valid, w.err, false);
            ok = Weather::fetch(g_settings);
            after = usageShape(w.valid, w.err, false);
            break;
        }
        case JOB_STATUS_CLAUDE: ok = VendorStatus::fetch(VendorStatus::CLAUDE); break;
        case JOB_STATUS_OPENAI: ok = VendorStatus::fetch(VendorStatus::OPENAI); break;
        default: break;
    }
    if (ok && (j == JOB_CLAUDE || j == JOB_CODEX)) UsageHistory::record(g_claude, g_codex);

    uint32_t now = millis();
    const FetchPolicy::State& pol = jobPolicy(j);
    uint32_t waitMs = ok ? jobIntervalMs(j)
                         : (pol.waitS ? pol.waitS * 1000UL : jobIntervalMs(j));
    g_jobDue[j] = now + waitMs;
    g_lastRefresh = now;
    Serial.printf_P(PSTR("[sched] %s %s → next in %lus (heap=%u maxblk=%u)\n"), kJobName[j],
                  ok ? "ok" : "fail", (unsigned long)(waitMs / 1000),
                  ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
    refreshScreen(before != after);
}

static void schedulerTick(uint32_t now) {
    if (g_apMode || WiFi.status() != WL_CONNECTED) return;
    if (now - g_lastJobStart < 3000) return;
    for (int i = 0; i < JOB_COUNT; i++) {
        Job j = (Job)i;
        if (!jobEnabled(j) || (int32_t)(now - g_jobDue[j]) < 0) continue;
        uint32_t blk = ESP.getMaxFreeBlockSize();
        if (blk < g_minMaxBlk) g_minMaxBlk = blk;
        if (j != JOB_WEATHER && blk < Api::kTlsFloor) {
            // Our own memory, not the upstream: defer briefly, never count it
            // as a failure.
            g_jobDue[j] = now + 5000;
            g_heapRefusals++;
            Serial.printf_P(PSTR("[sched] %s deferred: maxblk=%u < %u\n"), kJobName[j], blk,
                          (unsigned)Api::kTlsFloor);
            return;
        }
        g_lastJobStart = now;
        // Fetch activity: a 3-px dot in the bottom-right gap, cleared after.
        tft.fillRect(SCREEN_W - 5, 224, 3, 3, Theme::CORAL);
        runJob(j);
        tft.fillRect(SCREEN_W - 5, 224, 3, 3, Theme::BG);
        return;                                      // at most one job per pass
    }
}

// ── WiFi orchestration ───────────────────────────────────────────────────────

static void prepareSta(WiFiMode_t mode) {
    WiFi.persistent(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(150);
    WiFi.mode(mode);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(20.5);
    WiFi.setAutoReconnect(true);
    WiFi.hostname(MDNS_HOSTNAME);
    delay(100);
}

static bool tryConnect() {
    if (g_settings.wifiSSID.isEmpty()) return false;

    // Two clean attempts of 30 s each, then the setup AP comes up. The AP keeps
    // retrying the saved network in the background (see apStrandRetry), so a
    // router that is simply slow to boot no longer strands the device.
    constexpr int ATTEMPTS    = 2;
    constexpr int ATTEMPT_TICKS = 60;      // 60 × 500 ms = 30 s per attempt

    for (int attempt = 1; attempt <= ATTEMPTS; attempt++) {
        prepareSta(WIFI_STA);
        Serial.printf_P(PSTR("[wifi] attempt %d/%d: connecting to '%s'\n"), attempt, ATTEMPTS, g_settings.wifiSSID.c_str());
        WiFi.begin(g_settings.wifiSSID.c_str(), g_settings.wifiPass.c_str());

        for (int i = 0; i < ATTEMPT_TICKS; i++) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf_P(PSTR("[wifi] connected, IP=%s, RSSI=%d\n"), WiFi.localIP().toString().c_str(), WiFi.RSSI());
                return true;
            }
            Display::drawConnecting(g_settings.wifiSSID.c_str(), i + (attempt - 1) * ATTEMPT_TICKS);
            delay(500);
        }
        Serial.printf_P(PSTR("[wifi] attempt %d timed out (status=%d), backing off...\n"), attempt, WiFi.status());
        delay(2000);
    }
    Serial.println(F("[wifi] all attempts failed — falling back to AP mode"));
    return false;
}

static void startAPMode() {
    g_apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SETUP_AP_SSID);
    Display::drawSetupMode(SETUP_AP_SSID, WiFi.softAPIP().toString().c_str());
}

// STA is up: mDNS, time, and every fetch job due now.
static void onStaUp() {
    MDNS.begin(MDNS_HOSTNAME);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    Storage::applyTimezone(g_settings);
    for (int i = 0; i < 20 && !timeSynced(); i++) delay(250);
    mainTriggerRefresh();
}

// Stranded in setup-AP mode with saved credentials (e.g. the router was down
// at boot): every 180 s, try the saved network for 20 s while keeping the AP
// up. On success, drop the AP and carry on as a normal boot — no reboot.
static void apStrandRetry(uint32_t now) {
    static uint32_t lastTry = 0, tryStart = 0;
    if (!g_apMode || g_settings.wifiSSID.isEmpty()) return;
    if (tryStart == 0) {
        if (lastTry != 0 && now - lastTry < 180000UL) return;
        if (lastTry == 0) { lastTry = now; return; }   // first window starts at AP entry
        Serial.println(F("[wifi] AP-strand: retrying saved network"));
        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(g_settings.wifiSSID.c_str(), g_settings.wifiPass.c_str());
        tryStart = now;
        return;
    }
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0U)) {
        Serial.printf_P(PSTR("[wifi] AP-strand: joined, IP=%s\n"), WiFi.localIP().toString().c_str());
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        g_apMode = false;
        tryStart = 0;
        onStaUp();
        refreshScreen(true);
        return;
    }
    if (now - tryStart >= 20000UL) {
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        tryStart = 0;
        lastTry = now;
    }
}

// ── Setup / loop ─────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=== glimmer v" FW_VERSION " ==="));

    Display::begin();
    Display::drawSplash("booting...");
    delay(400);

    Storage::begin();
    g_settings = Storage::load();
    Display::setBrightness(g_settings.brightness);
    Display::setInvert(g_settings.invertDisplay);
    Weather::begin();
    UsageHistory::begin();

    Display::drawSplash("connecting WiFi");
    if (!tryConnect()) {
        startAPMode();
    } else {
        Display::drawSplash("Syncing time");
        onStaUp();
    }

    Web::begin(g_settings);

    if (!g_apMode) {
        // Personalized greeting splash
        if (!g_settings.userName.isEmpty()) {
            char greet[40];
            snprintf(greet, sizeof(greet), "Hi, %s", g_settings.userName.c_str());
            Display::drawSplash(greet);
            delay(900);
        }
        applyBrightness();
        refreshScreen(true);
    }
}

void loop() {
    Web::loop();
    if (!g_apMode) MDNS.update();

    uint32_t now = millis();

    apStrandRetry(now);

    // WiFi health check. "Healthy" means associated AND holding a DHCP lease.
    // A DHCP lease-renewal failure (common after ~10-12 h) leaves the link
    // associated — WiFi.status() stays WL_CONNECTED — but with IP 0.0.0.0, so
    // a status-only check never notices and the device silently stops talking
    // to the network. We treat "no IP" as lost too.
    //
    // Recovery is ALWAYS in-place (full RF reset, mirroring tryConnect()) and
    // never ESP.restart(): a software restart does NOT reset the ESP8266 WiFi
    // RF/calibration, so the post-restart connect reliably fails and the device
    // drops into AP mode — stuck there until a physical power-cycle. Retrying in
    // STA forever recovers cleanly whenever WiFi/DHCP returns. The 30 s arm
    // debounces brief glitches so a momentary blip doesn't bounce the radio.
    // (An OTA upload is handled inside a single Web::loop() call, so this
    // never fires mid-upload.)
    static uint32_t lastWifiCheck = 0;
    static uint32_t wifiRetrySince = 0;
    if (!g_apMode && now - lastWifiCheck >= 10000) {
        lastWifiCheck = now;
        bool healthy = (WiFi.status() == WL_CONNECTED)
                    && (WiFi.localIP() != IPAddress(0U));
        if (!healthy) {
            if (wifiRetrySince == 0) {
                wifiRetrySince = now;
                Serial.printf_P(PSTR("[wifi] unhealthy (status=%d ip=%s), waiting...\n"), WiFi.status(), WiFi.localIP().toString().c_str());
            } else if (now - wifiRetrySince >= 30000) {
                Serial.println(F("[wifi] reconnecting (full RF reset)..."));
                prepareSta(WIFI_STA);
                WiFi.begin(g_settings.wifiSSID.c_str(), g_settings.wifiPass.c_str());
                wifiRetrySince = now;
            }
        } else {
            if (wifiRetrySince != 0) {
                Serial.println(F("[wifi] reconnected"));
                mainTriggerRefresh();
            }
            wifiRetrySince = 0;
        }
    }

    schedulerTick(now);

    // If a push card is freshly active, snap to it immediately.
    // When it expires, advance to the next channel right away.
    static bool wasPushActive = false;
    ChannelCtx ctx = makeCtx();
    bool pushActive = !g_apMode && chPushEnabled(ctx);
    if (pushActive && !wasPushActive) {
        recomputeActive();
        for (int i = 0; i < g_activeCount; i++) {
            if (strcmp(kChannels[g_activeIdx[i]].name, "Push") == 0) {
                g_activePtr = i; break;
            }
        }
        drawActive();
        applyBrightness();                   // a dark night wakes for the card
        g_lastSlide = now;
    } else if (!pushActive && wasPushActive) {
        recomputeActive();
        drawActive();
        applyBrightness();
        g_lastSlide = now;
    }
    wasPushActive = pushActive;

    // Every 10 s: night window, staleness and status badges can change what
    // should be on screen (night face in/out) or how it looks (dimmed data).
    static uint32_t lastState = 0;
    static uint8_t  stateSig  = 0xFF;
    if (!g_apMode && now - lastState >= 10000UL) {
        lastState = now;
        uint8_t sig = (nightNow() ? 1 : 0)
                    | (Api::isStale(g_claude.lastOk, g_settings) ? 2 : 0)
                    | (Api::isStale(g_codex.lastOk, g_settings)  ? 4 : 0)
                    | (Weather::isStale(g_settings)              ? 8 : 0);
        if (sig != stateSig) {
            bool nightChanged = (sig ^ stateSig) & 1;
            stateSig = sig;
            applyBrightness();
            refreshScreen(nightChanged);
        }
    }

    // Channel auto-rotate — instant cut (no transition animation).
    uint32_t slideMs = (uint32_t)g_settings.channelSec * 1000UL;
    if (!g_apMode && g_settings.autoRotate && !g_nightFace && g_activeCount > 1
        && now - g_lastSlide >= slideMs) {
        g_lastSlide = now;
        // Pick up any settings toggles before deciding what's next.
        recomputeActive();
        if (g_activeCount > 0) g_activePtr = (g_activePtr + 1) % g_activeCount;
        drawActive();
    }

    // Partial-redraw tick — channels with a tick() callback are called at 5 Hz
    // (every 200 ms) to update only the regions that changed. NO full redraw
    // here — channels handle their own minimal repaints (see channel.h's
    // PARTIAL REDRAW DISCIPLINE comment).
    static uint32_t lastTick = 0;
    if (!g_apMode && g_activeCount > 0 && now - lastTick >= 200) {
        lastTick = now;
        auto tickFn = kChannels[g_activeIdx[g_activePtr]].tick;
        if (tickFn) tickFn(makeCtx());
    }

    // Indicator strip refreshes ~4 Hz (touches only y=230..231)
    static uint32_t lastInd = 0;
    if (!g_apMode && now - lastInd >= 250) {
        lastInd = now;
        drawIndicator(now);
    }

    delay(10);
}

// Settings were saved from the web UI: apply what can change at runtime.
void mainSettingsChanged() {
    Display::setInvert(g_settings.invertDisplay);
    Storage::applyTimezone(g_settings);
    applyBrightness();
    if (!g_apMode) refreshScreen(true);
}
