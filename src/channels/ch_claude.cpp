// Claude — design-true layout from ClaudeUsageScreen (screens.jsx:199-234).
//
// Fonts (regenerated via tools/genfonts.py at exact em-px sizes):
//   - "5-HOUR WINDOW" / "WEEKLY" / labels / sub-data : DMMono-11   (~13 px)
//   - countdowns "3h 2m"                              : Silkscreen-12 (~15 px)
//   - hero "%" digits                                 : VT323-86   (~70 px)
//   - inline "%" suffix on hero                       : VT323-44   (~36 px)
//   - weekly hero "%"                                 : VT323-44   (~36 px)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>

// ── tick() state cache ──
static float  s_sessPct  = -2.f;
static float  s_weekPct  = -2.f;
static char   s_sessReset[12] = "";
static char   s_weekSub[24]   = "";

bool chClaudeEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showClaude && !ctx.settings->claudeKey.isEmpty();
}

// ── Region paint helpers (also called from draw()) ──

static void paintSessReset(time_t resetEpoch) {
    String s = (resetEpoch > 0) ? Api::formatCountdown(resetEpoch) : String("--");
    tft.fillRect(SCREEN_W - 100, 28, 90, 16, Theme::BG);
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString(s, SCREEN_W - 12, 30);
    strncpy(s_sessReset, s.c_str(), sizeof(s_sessReset) - 1);
}

static void paintSessHero(float pct) {
    char pctBuf[8];
    if (pct < 0) snprintf(pctBuf, sizeof(pctBuf), "--");
    else         snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    // Clear hero band + suffix area
    tft.fillRect(10, 46, SCREEN_W - 20, 76, Theme::BG);

    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(pctBuf, 12, 46);
    int heroW = tft.textWidth(pctBuf);
    int heroH = tft.fontHeight();

    Display::useFont("VT323-44");
    tft.setTextColor(Theme::CORAL, Theme::BG);
    int pctY = 46 + (heroH - tft.fontHeight()) - 4;
    tft.drawString("%", 12 + heroW + 2, pctY);

    Display::pixelBar(12, 128, SCREEN_W - 24, 8,
                     pct < 0 ? 0 : pct, Theme::CORAL);
}

static void paintWeeklyHero(float pct) {
    tft.fillRect(SCREEN_W - 90, 148, 80, 36, Theme::BG);
    if (pct >= 0) {
        char wp[6]; snprintf(wp, sizeof(wp), "%d%%", (int)pct);
        Display::useFont("VT323-44");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::MINT, Theme::BG);
        tft.drawString(wp, SCREEN_W - 12, 150);
    }
    Display::pixelBar(12, 196, SCREEN_W - 24, 6,
                     pct < 0 ? 0 : pct, Theme::MINT);
}

static void paintWeeklySub(float pct, time_t weekReset) {
    String s;
    if (pct < 0) s = "--";
    else {
        s = String((int)pct) + "%";
        if (weekReset > 0) s += " \xC2\xB7 " + Api::formatCountdown(weekReset);
    }
    tft.fillRect(10, 168, 140, 16, Theme::BG);  // +2 px so DMMono-11 descenders fully clear
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(s, 12, 170);
    strncpy(s_weekSub, s.c_str(), sizeof(s_weekSub) - 1);
}

void chClaudeDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Claude", MoodId::NONE, "SONNET", Theme::CORAL);

    const ClaudeData& d = *ctx.claude;
    if (d.err[0]) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString(d.err, SCREEN_W/2, 100);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Refresh token in web UI", SCREEN_W/2, 124);
        s_sessPct = -2.f; s_weekPct = -2.f;
        s_sessReset[0] = 0; s_weekSub[0] = 0;
        return;
    }

    // Static labels (paint once)
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("5-HOUR WINDOW", 12, 30);

    tft.setTextDatum(TR_DATUM);
    tft.drawString("until reset", SCREEN_W - 12, 48);

    // Dynamic regions via helpers
    paintSessReset(d.sessionReset);
    paintSessHero(d.sessionPct);
    Display::dotsDivider(12, 146, SCREEN_W - 24);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("WEEKLY", 12, 154);

    paintWeeklyHero(d.weeklyPct);
    paintWeeklySub(d.weeklyPct, d.weeklyReset);

    // Seed cache for tick()
    s_sessPct = (d.sessionPct < 0) ? -2.f : d.sessionPct;
    s_weekPct = (d.weeklyPct  < 0) ? -2.f : d.weeklyPct;
}

void chClaudeTick(const ChannelCtx& ctx) {
    if (!ctx.claude) return;
    const ClaudeData& d = *ctx.claude;
    if (d.err[0]) return;  // error screen is static

    // Reset countdown — refresh whenever the formatted string would change
    String fresh = (d.sessionReset > 0) ? Api::formatCountdown(d.sessionReset) : String("--");
    if (strcmp(fresh.c_str(), s_sessReset) != 0) paintSessReset(d.sessionReset);

    // Hero % — repaint on meaningful change
    float p = (d.sessionPct < 0) ? -2.f : d.sessionPct;
    if (fabsf(p - s_sessPct) > 0.4f) {
        paintSessHero(d.sessionPct);
        s_sessPct = p;
    }

    // Weekly
    float wp = (d.weeklyPct < 0) ? -2.f : d.weeklyPct;
    if (fabsf(wp - s_weekPct) > 0.4f) {
        paintWeeklyHero(d.weeklyPct);
        s_weekPct = wp;
    }
    // Weekly sub (combo of pct + countdown)
    String subFresh;
    if (d.weeklyPct < 0) subFresh = "--";
    else {
        subFresh = String((int)d.weeklyPct) + "%";
        if (d.weeklyReset > 0) subFresh += " \xC2\xB7 " + Api::formatCountdown(d.weeklyReset);
    }
    if (strcmp(subFresh.c_str(), s_weekSub) != 0)
        paintWeeklySub(d.weeklyPct, d.weeklyReset);
}
