// Home — "vital signs" with partial-redraw discipline.
//
// chHomeDraw  → full paint, seeds cache.
// chHomeTick  → 5 Hz, diffs cache vs current, repaints only changed regions.
// Per channel.h's PARTIAL REDRAW DISCIPLINE — tick MUST NOT clear/fillScreen.
//
//   y=6..82    VT323-86 HH ink, ":" coral, MM amber          [hero clock]
//   y=14..50   right column temp (VT323-32 INK, TR)          [weather]
//   y=48..62   right column "feels XX°"  (DMMono-11 MUTED)
//   y=62..76   right column condition word (DMMono-11 INK_DIM)
//   y=92..104  date row "SUN MAY 17"
//   y=108      dots divider
//   y=114..128 CL meter row
//   y=132..146 CX meter row
//   y=152      dots divider
//   y=158..172 "TODAY" + "Nh LEFT"
//   y=178..    24-hour strip with current-hour amber marker
//   y=200..    footer "HOME | IP"

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "weather.h"
#include "weather_icons.h"
#include "clockfmt.h"
#include "chrome.h"
#include <ESP8266WiFi.h>
#include <time.h>
#include <math.h>


// ── File-static cache so tick() can diff vs last paint ──
static int     s_hh = -1, s_mm = -1, s_dayHour = -1;
static float   s_cl = -2.f, s_cx = -2.f;
static int     s_loadDot = -1;
static char    s_rain[32] = "";   // footer-left text on screen
static Cred    s_clCred = Cred::NOT_SET, s_cxCred = Cred::NOT_SET;
static int     s_rainMin = -1;
static float   s_tempC = -999.f;
static uint8_t s_code = 255;
// Clock x-geometry cached on first paint
static bool    s_geomReady = false;
static int     s_hhX = 8, s_colonX = 0, s_mmX = 0, s_digitW = 0;

bool chHomeEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showHome
        && time(nullptr) > 1000000000L;
}

static void clockGeom() {
    if (s_geomReady) return;
    Display::useFont("VT323-86");
    s_digitW = tft.textWidth("0");
    int colonW = tft.textWidth(":");
    s_hhX = 8;
    s_colonX = s_hhX + s_digitW * 2;
    s_mmX = s_colonX + colonW;
    s_geomReady = true;
}

// ── Per-region paint helpers ──

static void paintHH(int hh, bool h24) {
    clockGeom();
    char b[4]; ClockFmt::hourField(hh, h24, b, sizeof(b));
    tft.fillRect(s_hhX, 6, s_digitW * 2, 86, Theme::BG);
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(b, s_hhX, 6);
}

static void paintColon() {
    clockGeom();
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(":", s_colonX, 6);
}

static void paintMM(int mm) {
    clockGeom();
    char b[4]; snprintf_P(b, sizeof(b), PSTR("%02d"), mm);
    tft.fillRect(s_mmX, 6, s_digitW * 2, 86, Theme::BG);
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::AMBER, Theme::BG);
    tft.drawString(b, s_mmX, 6);
}

static void paintWeatherTemp(const WeatherData* w, bool f) {
    char tBuf[8];
    if (w && w->valid)
        snprintf_P(tBuf, sizeof(tBuf), PSTR("%.0f\xC2\xB0"), Weather::toDisplay(w->tempC, f));
    else
        snprintf_P(tBuf, sizeof(tBuf), PSTR("--\xC2\xB0"));
    tft.fillRect(SCREEN_W - 86, 14, 80, 36, Theme::BG);
    Display::useFont("VT323-32");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(tBuf, SCREEN_W - 10, 14);
}

static void paintWeatherFeels(const WeatherData* w, bool f) {
    char b[16];
    if (w && w->valid) {
        float fl = w->feelsC > -900 ? w->feelsC : w->tempC;
        snprintf_P(b, sizeof(b), PSTR("feels %.0f\xC2\xB0"), Weather::toDisplay(fl, f));
    } else {
        snprintf_P(b, sizeof(b), PSTR("feels --"));
    }
    tft.fillRect(SCREEN_W - 86, 48, 80, 14, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(b, SCREEN_W - 10, 48);
}

static void paintWeatherCondition(const WeatherData* w) {
    tft.fillRect(SCREEN_W - 86, 62, 86, 34, Theme::BG);
    if (w && w->valid) {
        WeatherIcon::draw(SCREEN_W - 36, 62, w->code, Theme::SKY, 2, !w->isDay);
        Display::useFont("DMMono-11");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(Weather::describe(w->code), SCREEN_W - 40, 78);
    } else {
        Display::useFont("DMMono-11");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString("--", SCREEN_W - 10, 62);
    }
}

static void paintMeter(int y, const char* tag, uint16_t tagColor,
                       float pct, uint16_t barColor, const char* val) {
    tft.fillRect(0, y, SCREEN_W, 16, Theme::BG);
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tagColor, Theme::BG);
    tft.drawString(tag, 10, y);

    Display::pixelBar(40, y + 3, 150, 7, pct, barColor);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(barColor, Theme::BG);
    tft.drawString(val, SCREEN_W - 10, y);
}

// Loading variant: tag on the left, a compact 3-dot chaser where the value
// would be. No bar — reads clearly as "not here yet".
static void paintMeterLoading(int y, const char* tag, uint16_t tagColor,
                              int lit, uint16_t accent) {
    tft.fillRect(0, y, SCREEN_W, 16, Theme::BG);
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tagColor, Theme::BG);
    tft.drawString(tag, 10, y);
    Display::loadingDots(SCREEN_W - 10 - 24, y + 4, lit, accent, 3);
}

// Credential variant: tag on the left, a short note where the bar and value
// would be ("add key · glimmer.local", "key expired · update").
static void paintMeterNote(int y, const char* tag, uint16_t tagColor,
                           const char* note, uint16_t noteColor) {
    tft.fillRect(0, y, SCREEN_W, 16, Theme::BG);
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tagColor, Theme::BG);
    tft.drawString(tag, 10, y);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(noteColor, Theme::BG);
    tft.drawString(note, SCREEN_W - 10, y);
}

static void paintUsageRow(int y, const char* tag, uint16_t tagColor, bool isClaude,
                          Cred cred, float pct, bool loading, int lit) {
    char note[40]; Chrome::credLine(isClaude, cred, note, sizeof(note));
    if (note[0]) {
        uint16_t c = cred == Cred::NOT_SET ? Theme::MUTED : credColor(cred);
        paintMeterNote(y, tag, cred == Cred::NOT_SET ? Theme::MUTED : tagColor, note, c);
        return;
    }
    if (loading) { paintMeterLoading(y, tag, tagColor, lit, tagColor); return; }
    char buf[8];
    if (pct >= 0) snprintf_P(buf, sizeof(buf), PSTR("%.0f%%"), pct);
    else          snprintf_P(buf, sizeof(buf), PSTR("--"));
    paintMeter(y, tag, tagColor, pct < 0 ? 0 : pct, Display::usageColor(pct), buf);
}

static void paintCL(const ChannelCtx& ctx, float cl, bool loading, int lit) {
    paintUsageRow(114, "CL", Theme::CORAL, true, ctx.claude->cred, cl, loading, lit);
}

static void paintCX(const ChannelCtx& ctx, float cx, bool loading, int lit) {
    paintUsageRow(132, "CX", Theme::LILAC, false, ctx.codex->cred, cx, loading, lit);
}

// Footer-left message, by priority: a credential problem (coral/amber) >
// a token expiring soon (amber) > the rain hint (sky) > "HOME".
static uint16_t footerText(const ChannelCtx& ctx, char* buf, size_t n) {
    struct P { const char* name; Cred cred; time_t exp; } ps[2] = {
        {"CLAUDE KEY",  ctx.claude->cred, 0},
        {"CODEX TOKEN", ctx.codex->cred,  ctx.codex->jwtExp},
    };
    for (const auto& p : ps) {
        if (!CredState::bad(p.cred)) continue;
        snprintf_P(buf, n, PSTR("%s %s"), p.name, p.cred == Cred::BLOCKED ? "BLOCKED" :
                                          p.cred == Cred::EXPIRED ? "EXPIRED" : "REJECTED");
        return credColor(p.cred);
    }
    for (const auto& p : ps) {
        if (p.cred != Cred::EXPIRING) continue;
        char d[8]; TimeUtil::shortDuration((long)(p.exp - time(nullptr)), d, sizeof(d));
        snprintf_P(buf, n, PSTR("%s %s LEFT"), p.name, d);
        return Theme::AMBER;
    }
    Weather::rainHint(*ctx.settings, buf, n);
    if (buf[0]) return Theme::SKY;
    snprintf_P(buf, n, PSTR("HOME"));
    return Theme::MUTED;
}

static void paintHourStrip(int curHour) {
    // Layout: TODAY label at y=154 (Silkscreen-12, spans y=154..167).
    // Strip baseline at y=180; current-hour marker at y=172..175; tallest tick
    // up to 6 px above baseline (y=174..180). Clear region y=170..182 keeps
    // 3 px clearance below TODAY.
    int stripX = 10, stripY = 180, stripW = SCREEN_W - 20;
    tft.fillRect(stripX, stripY - 10, stripW, 12, Theme::BG);
    tft.drawFastHLine(stripX, stripY, stripW, Theme::LINE);
    for (int h = 0; h < 24; h++) {
        int tx = stripX + (stripW * h) / 24;
        uint16_t c = (h == curHour) ? Theme::CORAL
                   : (h % 6 == 0)   ? Theme::INK_DIM
                                    : Theme::LINE;
        int th = (h == curHour) ? 6 : (h % 6 == 0 ? 3 : 2);
        tft.drawFastVLine(tx, stripY - th, th, c);
    }
    int curX = stripX + (stripW * curHour) / 24;
    tft.fillRect(curX - 1, stripY - 8, 3, 3, Theme::AMBER);

    // "Nh LEFT" right at y=154 (same line as TODAY label)
    char leftBuf[12];
    snprintf_P(leftBuf, sizeof(leftBuf), PSTR("%dh LEFT"), 23 - curHour);
    tft.fillRect(SCREEN_W - 80, 152, 76, 14, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(leftBuf, SCREEN_W - 10, 154);
}

static void paintFooterLeft(const char* text, uint16_t color) {
    tft.fillRect(0, 198, 138, 16, Theme::BG);           // stops short of the IP
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, Theme::BG);
    tft.drawString(text, 10, 200);
    strncpy(s_rain, text, sizeof(s_rain) - 1);
}

// ── Full repaint ──

void chHomeDraw(const ChannelCtx& ctx) {
    Display::clear();
    // Note: the "vital signs" Home design has no top status bar — the clock
    // takes the top of the canvas, and the chrome is the footer (HOME | IP).

    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);

    // Hero clock
    paintHH(tmv.tm_hour, ctx.settings->clock24h);
    paintColon();
    paintMM(tmv.tm_min);

    // Weather column
    const WeatherData* w = &Weather::snapshot();
    bool f = ctx.settings && ctx.settings->useFahrenheit;
    paintWeatherTemp(w, f);
    paintWeatherFeels(w, f);
    paintWeatherCondition(w);

    // Date row (static-ish, only changes day-over-day)
    char dateBuf[24];
    strftime(dateBuf, sizeof(dateBuf), "%a %b %d", &tmv);
    for (int i = 0; dateBuf[i] && i < 20; i++) {
        if (dateBuf[i] >= 'a' && dateBuf[i] <= 'z') dateBuf[i] -= 32;
    }
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(dateBuf, 10, 92);

    Display::dotsDivider(10, 108, SCREEN_W - 20);

    // AI meters — "loading" only for a *configured* side (an unconfigured slot
    // stays valid=false/err="" and must read as "--", not a perpetual loader).
    const bool clLoading = ctx.claude && !ctx.settings->claudeKey.isEmpty()   && claudeLoading(*ctx.claude);
    const bool cxLoading = ctx.codex  && !ctx.settings->codexToken.isEmpty()  && codexLoading(*ctx.codex);
    float cl = ctx.claude ? Api::claudeHeroPct(*ctx.settings, *ctx.claude) : -1.f;
    float cx = ctx.codex  ? Api::codexHeroPct(*ctx.settings, *ctx.codex) : -1.f;
    const int lit = (ctx.now_ms / 150) % 3;
    paintCL(ctx, cl, clLoading, lit);
    paintCX(ctx, cx, cxLoading, lit);
    s_clCred = ctx.claude->cred; s_cxCred = ctx.codex->cred;
    s_loadDot = (clLoading || cxLoading) ? lit : -1;

    Display::dotsDivider(10, 152, SCREEN_W - 20);

    // "TODAY" label at y=154 — paintHourStrip clears from y=170 so no overlap.
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("TODAY", 10, 154);
    paintHourStrip(tmv.tm_hour);

    // Footer
    char foot[32]; uint16_t fc = footerText(ctx, foot, sizeof(foot));
    paintFooterLeft(foot, fc);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(WiFi.localIP().toString(), SCREEN_W - 10, 200);

    // ── Seed cache ──
    s_hh = tmv.tm_hour; s_mm = tmv.tm_min; s_dayHour = tmv.tm_hour;
    s_cl = (cl < 0) ? -2.f : cl;
    s_cx = (cx < 0) ? -2.f : cx;
    if (w && w->valid) { s_tempC = w->tempC; s_code = w->code; }
    else               { s_tempC = -999.f; s_code = 255; }
}

// ── Tick: 5 Hz, region-only repaints ──

void chHomeTick(const ChannelCtx& ctx) {
    time_t now = time(nullptr);
    if (now < 1000000000L) return;
    struct tm tmv; localtime_r(&now, &tmv);

    // Clock
    if (tmv.tm_min != s_mm) { paintMM(tmv.tm_min); s_mm = tmv.tm_min; }
    if (tmv.tm_hour != s_hh) { paintHH(tmv.tm_hour, ctx.settings->clock24h); s_hh = tmv.tm_hour; }
    if (tmv.tm_min != s_rainMin) {
        char foot[32]; uint16_t fc = footerText(ctx, foot, sizeof(foot));
        if (strcmp(foot, s_rain) != 0) paintFooterLeft(foot, fc);
        s_rainMin = tmv.tm_min;
    }
    if (tmv.tm_hour != s_dayHour) {
        paintHourStrip(tmv.tm_hour);
        s_dayHour = tmv.tm_hour;
    }

    // Weather (only repaint on meaningful change)
    const WeatherData* w = &Weather::snapshot();
    bool f = ctx.settings && ctx.settings->useFahrenheit;
    if (w && w->valid) {
        if (fabsf(w->tempC - s_tempC) > 0.4f) {
            paintWeatherTemp(w, f);
            paintWeatherFeels(w, f);
            s_tempC = w->tempC;
        }
        if (w->code != s_code) {
            paintWeatherCondition(w);
            s_code = w->code;
        }
    }

    // AI meters — chase dots while a side is still loading; else hysteresis on
    // ±0.4% so noise doesn't thrash.
    const bool clLoading = ctx.claude && !ctx.settings->claudeKey.isEmpty()  && claudeLoading(*ctx.claude);
    const bool cxLoading = ctx.codex  && !ctx.settings->codexToken.isEmpty() && codexLoading(*ctx.codex);
    const float cl = ctx.claude ? Api::claudeHeroPct(*ctx.settings, *ctx.claude) : -1.f;
    const float cx = ctx.codex  ? Api::codexHeroPct(*ctx.settings, *ctx.codex) : -1.f;
    const int lit = (ctx.now_ms / 150) % 3;

    // A credential change swaps the bar for a note (or back).
    if (ctx.claude->cred != s_clCred) { s_cl = -3.f; s_clCred = ctx.claude->cred; }
    if (ctx.codex->cred  != s_cxCred) { s_cx = -3.f; s_cxCred = ctx.codex->cred; }
    if (clLoading) {
        if (lit != s_loadDot) paintCL(ctx, cl, true, lit);
        s_cl = -2.f;                                  // force repaint when data lands
    } else {
        float cl_eff = (cl < 0) ? -2.f : cl;
        if (fabsf(cl_eff - s_cl) > 0.4f) { paintCL(ctx, cl, false, lit); s_cl = cl_eff; }
    }
    if (cxLoading) {
        if (lit != s_loadDot) paintCX(ctx, cx, true, lit);
        s_cx = -2.f;
    } else {
        float cx_eff = (cx < 0) ? -2.f : cx;
        if (fabsf(cx_eff - s_cx) > 0.4f) { paintCX(ctx, cx, false, lit); s_cx = cx_eff; }
    }
    if (clLoading || cxLoading) s_loadDot = lit;
}
