// SmallTV — main orchestrator.
//
// Architecture:
//   core/    hardware + I/O (display, wifi, time, storage, web, OTA)
//   data/    external API clients (claude, codex, ...)
//   channels/ self-contained renderers, registered in kChannels[]
//
// Adding a channel = drop one .cpp into src/channels/ + add a row to kChannels[].

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
#include "channel.h"
#include "transitions.h"
#include "mood.h"

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
extern void chPushTick       (const ChannelCtx&);
extern void chClockTick      (const ChannelCtx&);
extern void chHomeTick       (const ChannelCtx&);
extern void chClaudeTick     (const ChannelCtx&);
extern void chCodexTick      (const ChannelCtx&);
extern void chAiDashTick     (const ChannelCtx&);
extern void chWeatherTick    (const ChannelCtx&);
extern void chForecastTick   (const ChannelCtx&);
extern void chInfoTick       (const ChannelCtx&);
extern void weatherTick      (const Settings&);

static const Channel kChannels[] = {
    //  name        enabled              draw                  tick
    { "Push",     chPushEnabled,     chPushDraw,     chPushTick     },
    { "Home",     chHomeEnabled,     chHomeDraw,     chHomeTick     },
    { "Claude",   chClaudeEnabled,   chClaudeDraw,   chClaudeTick   },
    { "Codex",    chCodexEnabled,    chCodexDraw,    chCodexTick    },
    { "AI",       chAiDashEnabled,   chAiDashDraw,   chAiDashTick   },
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

static int         g_activeIdx[8];        // indices into kChannels[] that are currently enabled
static int         g_activeCount = 0;
static int         g_activePtr   = 0;     // which active channel is on screen

static uint32_t    g_lastRefresh = 0;
static uint32_t    g_lastSlide   = 0;
static uint32_t    g_lastTick    = 0;

// ── Helpers ──────────────────────────────────────────────────────────────────

static ChannelCtx makeCtx() {
    return ChannelCtx { &g_settings, &g_claude, &g_codex, millis() };
}

static void recomputeActive() {
    ChannelCtx ctx = makeCtx();
    g_activeCount = 0;
    for (int i = 0; i < kChannelCount && g_activeCount < 8; i++) {
        if (kChannels[i].enabled(ctx)) g_activeIdx[g_activeCount++] = i;
    }
    if (g_activePtr >= g_activeCount) g_activePtr = 0;
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

// 2-px progress strip at y=230. Fills in current channel's theme color as
// the slide window elapses. Hidden while Push card is active (Push is
// interruption, not rotation).
static void drawIndicator(uint32_t now) {
    using namespace Layout;
    if (g_apMode || g_activeCount <= 1) return;
    const char* name = kChannels[g_activeIdx[g_activePtr]].name;
    if (!strcmp(name, "Push")) return;

    uint32_t slideMs = (uint32_t)g_settings.channelSec * 1000UL;
    uint32_t elapsed = now - g_lastSlide;
    if (elapsed > slideMs) elapsed = slideMs;
    int w = (int)((uint64_t)SCREEN_W * elapsed / slideMs);

    tft.fillRect(0, INDICATOR_Y, SCREEN_W, INDICATOR_H, Theme::PANEL);
    if (w > 0) tft.fillRect(0, INDICATOR_Y, w, INDICATOR_H, Theme::channelColor(name));
}

// ── WiFi orchestration ───────────────────────────────────────────────────────

static bool tryConnect() {
    if (g_settings.wifiSSID.isEmpty()) return false;

    // Multi-attempt connect with clean state between tries. Total budget ~3.5 min
    // before falling back to AP mode. Each attempt: 60 s with full disconnect+mode-flip.
    constexpr int ATTEMPTS    = 3;
    constexpr int ATTEMPT_TICKS = 120;     // 120 × 500 ms = 60 s per attempt

    for (int attempt = 1; attempt <= ATTEMPTS; attempt++) {
        WiFi.persistent(false);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(150);
        WiFi.mode(WIFI_STA);
        WiFi.setSleepMode(WIFI_NONE_SLEEP);
        WiFi.setOutputPower(20.5);
        WiFi.setAutoReconnect(true);
        WiFi.hostname(MDNS_HOSTNAME);
        delay(100);

        Serial.printf("[wifi] attempt %d/%d: connecting to '%s'\n",
                      attempt, ATTEMPTS, g_settings.wifiSSID.c_str());
        WiFi.begin(g_settings.wifiSSID.c_str(), g_settings.wifiPass.c_str());

        for (int i = 0; i < ATTEMPT_TICKS; i++) {
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[wifi] connected, IP=%s, RSSI=%d\n",
                              WiFi.localIP().toString().c_str(), WiFi.RSSI());
                return true;
            }
            Display::drawConnecting(g_settings.wifiSSID.c_str(), i + (attempt - 1) * ATTEMPT_TICKS);
            delay(500);
        }
        Serial.printf("[wifi] attempt %d timed out (status=%d), backing off...\n",
                      attempt, WiFi.status());
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

// ── Refresh cycle ────────────────────────────────────────────────────────────

// Tiny gap between TLS calls so BearSSL fully tears down + heap settles.
// Without this, back-to-back fetches sometimes fail handshake (Auth -1).
static void apiYieldGap() { yield(); delay(150); }

static void refreshAll() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!g_settings.claudeKey.isEmpty()) {
        Api::fetchClaude(g_settings, g_claude);
        apiYieldGap();
    }
    if (!g_settings.codexToken.isEmpty()) {
        Api::fetchCodex(g_settings, g_codex);
        apiYieldGap();
    }
    // Fetch weather if ANY channel that uses it is enabled (Home + Forecast also
    // consume the snapshot, not just the Weather channel itself).
    if (g_settings.showWeather || g_settings.showHome || g_settings.showForecast) {
        weatherTick(g_settings);
        apiYieldGap();
    }
    recomputeActive();
    drawActive();
}

// ── Setup / loop ─────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println(F("\n=== SmallTV v" FW_VERSION " ==="));

    Display::begin();
    Display::drawSplash("booting...");
    delay(400);

    Storage::begin();
    g_settings = Storage::load();
    Display::setBrightness(g_settings.brightness);
    Display::setInvert(g_settings.invertDisplay);

    Display::drawSplash("connecting WiFi");
    if (!tryConnect()) {
        startAPMode();
    } else {
        MDNS.begin(MDNS_HOSTNAME);
        Display::drawSplash("Syncing time");
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        // POSIX TZ: tzMinutes (signed minutes east of UTC) takes precedence
        // over the legacy hour-only tzOffset. POSIX expresses offset as
        // "minutes to ADD to local time to get UTC" → invert the sign.
        int signedMin = (g_settings.tzMinutes != 0)
                      ? (int)g_settings.tzMinutes
                      : (int)g_settings.tzOffset * 60;
        int posixMin  = -signedMin;
        int posixH    = posixMin / 60;
        int posixM    = posixMin % 60; if (posixM < 0) posixM = -posixM;
        char tzBuf[16];
        snprintf(tzBuf, sizeof(tzBuf), "UTC%+d:%02d", posixH, posixM);
        setenv("TZ", tzBuf, 1);
        tzset();
        for (int i = 0; i < 20 && time(nullptr) < 1000000000L; i++) delay(250);
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
        Display::drawSplash("Fetching...");
        refreshAll();
        g_lastRefresh = millis();
        g_lastSlide   = millis();
    }
}

void loop() {
    Web::loop();
    if (!g_apMode) MDNS.update();

    uint32_t now = millis();

    // Periodic API refresh
    uint32_t refreshMs = (uint32_t)g_settings.refreshMin * 60000UL;
    if (!g_apMode && now - g_lastRefresh >= refreshMs) {
        g_lastRefresh = now;
        refreshAll();
        g_lastSlide = now;
    }

    // If a push card is freshly active, snap to it immediately
    static bool wasPushActive = false;
    ChannelCtx ctx = makeCtx();
    bool pushActive = chPushEnabled(ctx);
    if (pushActive && !wasPushActive) {
        recomputeActive();
        for (int i = 0; i < g_activeCount; i++) {
            if (strcmp(kChannels[g_activeIdx[i]].name, "Push") == 0) {
                g_activePtr = i; break;
            }
        }
        drawActive();
        g_lastSlide = now;
    }
    wasPushActive = pushActive;

    // Auto-brightness every 60s
    static uint32_t lastBright = 0;
    if (now - lastBright >= 60000UL) {
        lastBright = now;
        time_t t = time(nullptr);
        if (t > 1000000000L) {
            struct tm tm; localtime_r(&t, &tm);
            int b = Mood::suggestedBrightness(tm.tm_hour, g_settings.brightness,
                                              g_settings.nightDim, g_settings.nightBright,
                                              g_settings.nightStart, g_settings.nightEnd);
            Display::setBrightness(b);
        }
    }

    // Channel auto-rotate — instant cut (no transition animation). Premium
    // devices do this; the single fillScreen+redraw in drawActive() is the
    // only flash, and it lasts <30 ms.
    uint32_t slideMs = (uint32_t)g_settings.channelSec * 1000UL;
    if (!g_apMode && g_settings.autoRotate && g_activeCount > 1
        && now - g_lastSlide >= slideMs) {
        g_lastSlide = now;
        // Pick up any settings toggles before deciding what's next.
        recomputeActive();
        g_activePtr = (g_activePtr + 1) % g_activeCount;
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
