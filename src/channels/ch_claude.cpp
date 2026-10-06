// Claude — design-true layout from ClaudeUsageScreen (screens.jsx:199-234).
//
// Fonts (regenerated via tools/genfonts.py at exact em-px sizes):
//   - "5-HOUR WINDOW" / "WEEKLY" / labels / sub-data : DMMono-11   (~13 px)
//   - countdowns "3h 2m"                              : VT323-32    (~26 px)
//   - hero "%" digits                                 : VT323-86   (~70 px)
//   - inline "%" suffix on hero                       : VT323-44   (~36 px)
//
//   y=176..218  bottom rows: weekly PACE (when the history has a trend) then
//               per-model windows (2 when PACE shows, else 3).

#include "channel.h"
#include "chrome.h"
#include "display.h"
#include "history.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>
#include <time.h>

// ── tick() state cache (position-based, not source-based) ──
static float  s_heroPct  = -2.f;
static float  s_secPct   = -2.f;
static bool   s_stale    = false;
static char   s_heroReset[12] = "";
static char   s_secSub[24]    = "";
static float  s_modelPct[3]       = {-2.f, -2.f, -2.f};
static char   s_modelLabel[3][12] = {"", "", ""};
static char   s_pace[20]      = "";
static MetaSlot s_meta;
static int    s_loadDot = -1;

bool chClaudeEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showClaude && !ctx.settings->claudeKey.isEmpty();
}

// Lowest-remaining model label — the status bar's normal right meta.
static const char* modelTag(const ClaudeData& d) {
    if (!d.valid || !d.models[0].label[0]) return "";
    int best = 0;
    for (int i = 1; i < 3; i++)
        if (d.models[i].pct >= 0 && d.models[i].pct < d.models[best].pct) best = i;
    return d.models[best].label;
}

static MetaSlot metaFor(const ChannelCtx& ctx) {
    return usageMeta(*ctx.settings, ctx.claude->lastOk, VendorStatus::CLAUDE,
                     modelTag(*ctx.claude));
}

static void paceFor(const ClaudeData& d, char* buf, size_t n) {
    History::Pace p = History::pace(UsageHistory::ring(), History::CLAUDE_WEEK,
                                    d.weeklyPct, time(nullptr), d.weeklyReset);
    History::paceText(p, buf, n);
}

// ── Region paint helpers (also called from draw()) ──

static void paintSessReset(time_t resetEpoch) {
    String s = (resetEpoch > 0) ? Api::formatCountdown(resetEpoch) : String("--");
    tft.fillRect(SCREEN_W - 110, 26, 100, 28, Theme::BG);
    Display::useFont("VT323-32");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(s, SCREEN_W - 12, 26);
    strncpy(s_heroReset, s.c_str(), sizeof(s_heroReset) - 1);
}

static void paintSessHero(float pct, bool stale) {
    char pctBuf[8];
    if (pct < 0) snprintf(pctBuf, sizeof(pctBuf), "--");
    else         snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    // Clear hero band + suffix area
    tft.fillRect(10, 46, SCREEN_W - 20, 76, Theme::BG);

    // Stale data stays visible but dimmed — it is not a live reading.
    uint16_t uc = stale ? Theme::MUTED : Display::usageColor(pct);
    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(uc, Theme::BG);
    tft.drawString(pctBuf, 12, 46);
    int heroW = tft.textWidth(pctBuf);
    int heroH = tft.fontHeight();

    Display::useFont("VT323-44");
    tft.setTextColor(uc, Theme::BG);
    int pctY = 46 + (heroH - tft.fontHeight()) - 4;
    tft.drawString("%", 12 + heroW + 2, pctY);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString("CLAUDE", SCREEN_W - 12, 76);

    Display::pixelBar(12, 128, SCREEN_W - 24, 8,
                     pct < 0 ? 0 : pct, uc);
}

static void paintWeeklyCompact(const char* label, float pct, time_t weekReset) {
    tft.fillRect(0, 150, SCREEN_W, 20, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(label, 12, 150);

    String s;
    if (pct < 0) s = "--";
    else {
        s = String((int)pct) + "%";
        if (weekReset > 0) s += " \xC2\xB7 " + Api::formatCountdown(weekReset);
    }
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(s, SCREEN_W - 12, 150);
    strncpy(s_secSub, s.c_str(), sizeof(s_secSub) - 1);

    uint16_t uc = Display::usageColor(pct);
    Display::pixelBar(12, 164, SCREEN_W - 24, 4, pct < 0 ? 0 : pct, uc);
}

static void paintBottomRows(const ClaudeData& d, const char* pace) {
    const int startY = 176, rowH = 14;
    tft.fillRect(0, startY, SCREEN_W, rowH * 3, Theme::BG);
    Display::useFont("DMMono-11");
    int row = 0;
    if (pace[0]) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("WK PACE", 12, startY);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(strncmp(pace, "EMPTY", 5) == 0 ? Theme::CORAL : Theme::INK_DIM, Theme::BG);
        tft.drawString(pace, SCREEN_W - 12, startY);
        row = 1;
    }
    for (int i = 0; i < 3 && row < 3; i++) {
        if (d.models[i].pct < 0 || !d.models[i].label[0]) continue;
        int y = startY + row * rowH;
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(d.models[i].label, 12, y);
        uint16_t mc = Display::usageColor(d.models[i].pct);
        Display::pixelBar(100, y + 3, 80, 5, d.models[i].pct, mc);
        char buf[6]; snprintf(buf, sizeof(buf), "%d%%", (int)d.models[i].pct);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(mc, Theme::BG);
        tft.drawString(buf, SCREEN_W - 12, y);
        row++;
    }
    for (int i = 0; i < 3; i++) {
        s_modelPct[i] = d.models[i].pct < 0 ? -2.f : d.models[i].pct;
        strncpy(s_modelLabel[i], d.models[i].label, 11);
        s_modelLabel[i][11] = '\0';
    }
    strncpy(s_pace, pace, sizeof(s_pace) - 1);
}

void chClaudeDraw(const ChannelCtx& ctx) {
    Display::clear();

    const ClaudeData& d = *ctx.claude;
    s_meta = metaFor(ctx);
    Display::statusBar("Claude", s_meta.text, Theme::CORAL, s_meta.color);

    if (d.err[0]) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString(d.err, SCREEN_W/2, 100);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(errorHint(d.authErr), SCREEN_W/2, 124);
        s_heroPct = -2.f; s_secPct = -2.f;
        s_heroReset[0] = 0; s_secSub[0] = 0;
        return;
    }
    if (!d.valid) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Loading", SCREEN_W/2, 100);
        Display::loadingDots(SCREEN_W/2 - 21, 128, 0, Theme::CORAL);
        s_loadDot = 0;
        s_heroPct = -2.f; s_secPct = -2.f;
        s_heroReset[0] = 0; s_secSub[0] = 0;
        return;
    }

    const bool swapped = ctx.settings->claudeWeeklyHero;
    const float heroPct  = swapped ? d.weeklyPct   : d.sessionPct;
    const float secPct   = swapped ? d.sessionPct  : d.weeklyPct;
    const time_t heroRst = swapped ? d.weeklyReset : d.sessionReset;
    const time_t secRst  = swapped ? d.sessionReset: d.weeklyReset;
    s_stale = Api::isStale(d.lastOk, *ctx.settings);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(swapped ? "WEEKLY" : "5-HOUR WINDOW", 12, 32);

    paintSessReset(heroRst);
    paintSessHero(heroPct, s_stale);
    Display::dotsDivider(12, 146, SCREEN_W - 24);

    const char* secLabel = swapped ? "5-HOUR WINDOW" : "WEEKLY";
    paintWeeklyCompact(secLabel, secPct, secRst);
    Display::dotsDivider(12, 170, SCREEN_W - 24);
    char pace[20]; paceFor(d, pace, sizeof(pace));
    paintBottomRows(d, pace);

    s_heroPct = (heroPct < 0) ? -2.f : heroPct;
    s_secPct  = (secPct  < 0) ? -2.f : secPct;
}

void chClaudeTick(const ChannelCtx& ctx) {
    if (!ctx.claude) return;
    const ClaudeData& d = *ctx.claude;
    if (d.err[0]) return;
    if (!d.valid) {                       // loading — sweep the chase dots
        int lit = (ctx.now_ms / 150) % 5;
        if (lit != s_loadDot) {
            Display::loadingDots(SCREEN_W/2 - 21, 128, lit, Theme::CORAL);
            s_loadDot = lit;
        }
        return;
    }

    // Countdown / badge / pace strings only change on a minute boundary;
    // recompute (and heap-allocate) them only then, not on every 5 Hz tick.
    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    static int s_cdMin = -1;
    const bool minTick = (tm.tm_min != s_cdMin);

    const bool swapped = ctx.settings->claudeWeeklyHero;
    const float heroPct  = swapped ? d.weeklyPct   : d.sessionPct;
    const float secPct   = swapped ? d.sessionPct  : d.weeklyPct;
    const time_t heroRst = swapped ? d.weeklyReset : d.sessionReset;
    const time_t secRst  = swapped ? d.sessionReset: d.weeklyReset;

    if (minTick) {
        MetaSlot m = metaFor(ctx);
        if (m != s_meta) { Display::statusMeta(m.text, Theme::CORAL, m.color); s_meta = m; }
        String fresh = (heroRst > 0) ? Api::formatCountdown(heroRst) : String("--");
        if (strcmp(fresh.c_str(), s_heroReset) != 0) paintSessReset(heroRst);
    }

    float p = (heroPct < 0) ? -2.f : heroPct;
    bool stale = Api::isStale(d.lastOk, *ctx.settings);
    if (fabsf(p - s_heroPct) > 0.4f || stale != s_stale) {
        paintSessHero(heroPct, stale);
        s_heroPct = p;
        s_stale = stale;
    }

    // Secondary compact row — pct moves on refresh, countdown at minute ticks.
    float sp = (secPct < 0) ? -2.f : secPct;
    bool secDirty = fabsf(sp - s_secPct) > 0.4f;
    if (!secDirty && minTick) {
        String subFresh;
        if (secPct < 0) subFresh = "--";
        else {
            subFresh = String((int)secPct) + "%";
            if (secRst > 0) subFresh += " \xC2\xB7 " + Api::formatCountdown(secRst);
        }
        if (strcmp(subFresh.c_str(), s_secSub) != 0) secDirty = true;
    }
    if (secDirty) {
        const char* secLabel = swapped ? "5-HOUR WINDOW" : "WEEKLY";
        paintWeeklyCompact(secLabel, secPct, secRst);
        s_secPct = sp;
    }

    bool rowsDirty = false;
    for (int i = 0; i < 3; i++) {
        float mp = d.models[i].pct < 0 ? -2.f : d.models[i].pct;
        if (fabsf(mp - s_modelPct[i]) > 0.4f) rowsDirty = true;
        if (strcmp(d.models[i].label, s_modelLabel[i]) != 0) rowsDirty = true;
    }
    char pace[20] = "";
    if (rowsDirty || minTick) {
        paceFor(d, pace, sizeof(pace));
        if (strcmp(pace, s_pace) != 0) rowsDirty = true;
    }
    if (rowsDirty) paintBottomRows(d, pace);

    s_cdMin = tm.tm_min;
}
