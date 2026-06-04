// Claude — design-true layout from ClaudeUsageScreen (screens.jsx:199-234).
//
// Fonts (regenerated via tools/genfonts.py at exact em-px sizes):
//   - "5-HOUR WINDOW" / "WEEKLY" / labels / sub-data : DMMono-11   (~13 px)
//   - countdowns "3h 2m"                              : VT323-32    (~26 px)
//   - hero "%" digits                                 : VT323-86   (~70 px)
//   - inline "%" suffix on hero                       : VT323-44   (~36 px)
//   - weekly hero "%"                                 : VT323-44   (~36 px)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>

// ── tick() state cache (position-based, not source-based) ──
static float  s_heroPct  = -2.f;
static float  s_secPct   = -2.f;
static char   s_heroReset[12] = "";
static char   s_secSub[24]    = "";
static float  s_modelPct[3]       = {-2.f, -2.f, -2.f};
static char   s_modelLabel[3][12] = {"", "", ""};

bool chClaudeEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showClaude && !ctx.settings->claudeKey.isEmpty();
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

static void paintSessHero(float pct) {
    char pctBuf[8];
    if (pct < 0) snprintf(pctBuf, sizeof(pctBuf), "--");
    else         snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    // Clear hero band + suffix area
    tft.fillRect(10, 46, SCREEN_W - 20, 76, Theme::BG);

    uint16_t uc = Display::usageColor(pct);
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

static void paintModelRows(const ClaudeData& d) {
    const int startY = 176, rowH = 14;
    tft.fillRect(0, startY, SCREEN_W, rowH * 3, Theme::BG);
    Display::useFont("DMMono-11");
    for (int i = 0; i < 3; i++) {
        if (d.models[i].pct < 0 || !d.models[i].label[0]) continue;
        int y = startY + i * rowH;
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(d.models[i].label, 12, y);
        uint16_t mc = Display::usageColor(d.models[i].pct);
        Display::pixelBar(100, y + 3, 80, 5, d.models[i].pct, mc);
        char buf[6]; snprintf(buf, sizeof(buf), "%d%%", (int)d.models[i].pct);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(mc, Theme::BG);
        tft.drawString(buf, SCREEN_W - 12, y);
    }
    for (int i = 0; i < 3; i++) {
        s_modelPct[i] = d.models[i].pct < 0 ? -2.f : d.models[i].pct;
        strncpy(s_modelLabel[i], d.models[i].label, 11);
        s_modelLabel[i][11] = '\0';
    }
}

void chClaudeDraw(const ChannelCtx& ctx) {
    Display::clear();

    const ClaudeData& d = *ctx.claude;

    const char* modelTag = "";
    if (d.valid && d.models[0].label[0]) {
        int best = 0;
        const bool consumed = ctx.settings->usageShowConsumed;
        for (int i = 1; i < 3; i++) {
            if (d.models[i].pct < 0) continue;
            // Surface the model closest to its limit: highest consumed / lowest remaining.
            bool more = consumed ? (d.models[i].pct > d.models[best].pct)
                                 : (d.models[i].pct < d.models[best].pct);
            if (more) best = i;
        }
        modelTag = d.models[best].label;
    }
    Display::statusBar("Claude", modelTag, Theme::CORAL);

    if (d.err[0]) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString(d.err, SCREEN_W/2, 100);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Refresh token in web UI", SCREEN_W/2, 124);
        s_heroPct = -2.f; s_secPct = -2.f;
        s_heroReset[0] = 0; s_secSub[0] = 0;
        return;
    }

    const bool swapped = ctx.settings->claudeWeeklyHero;
    const float heroPct  = swapped ? d.weeklyPct   : d.sessionPct;
    const float secPct   = swapped ? d.sessionPct  : d.weeklyPct;
    const time_t heroRst = swapped ? d.weeklyReset : d.sessionReset;
    const time_t secRst  = swapped ? d.sessionReset: d.weeklyReset;

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(swapped ? "WEEKLY" : "5-HOUR WINDOW", 12, 32);

    paintSessReset(heroRst);
    paintSessHero(heroPct);
    Display::dotsDivider(12, 146, SCREEN_W - 24);

    const char* secLabel = swapped ? "5-HOUR WINDOW" : "WEEKLY";
    paintWeeklyCompact(secLabel, secPct, secRst);
    Display::dotsDivider(12, 170, SCREEN_W - 24);
    paintModelRows(d);

    s_heroPct = (heroPct < 0) ? -2.f : heroPct;
    s_secPct  = (secPct  < 0) ? -2.f : secPct;
}

void chClaudeTick(const ChannelCtx& ctx) {
    if (!ctx.claude) return;
    const ClaudeData& d = *ctx.claude;
    if (d.err[0]) return;

    const bool swapped = ctx.settings->claudeWeeklyHero;
    const float heroPct  = swapped ? d.weeklyPct   : d.sessionPct;
    const float secPct   = swapped ? d.sessionPct  : d.weeklyPct;
    const time_t heroRst = swapped ? d.weeklyReset : d.sessionReset;
    const time_t secRst  = swapped ? d.sessionReset: d.weeklyReset;

    String fresh = (heroRst > 0) ? Api::formatCountdown(heroRst) : String("--");
    if (strcmp(fresh.c_str(), s_heroReset) != 0) paintSessReset(heroRst);

    float p = (heroPct < 0) ? -2.f : heroPct;
    if (fabsf(p - s_heroPct) > 0.4f) {
        paintSessHero(heroPct);
        s_heroPct = p;
    }

    String subFresh;
    if (secPct < 0) subFresh = "--";
    else {
        subFresh = String((int)secPct) + "%";
        if (secRst > 0) subFresh += " \xC2\xB7 " + Api::formatCountdown(secRst);
    }
    if (strcmp(subFresh.c_str(), s_secSub) != 0) {
        const char* secLabel = swapped ? "5-HOUR WINDOW" : "WEEKLY";
        paintWeeklyCompact(secLabel, secPct, secRst);
    }

    bool modelDirty = false;
    for (int i = 0; i < 3; i++) {
        float mp = d.models[i].pct < 0 ? -2.f : d.models[i].pct;
        if (fabsf(mp - s_modelPct[i]) > 0.4f) modelDirty = true;
        if (strcmp(d.models[i].label, s_modelLabel[i]) != 0) modelDirty = true;
    }
    if (modelDirty) paintModelRows(d);
}
