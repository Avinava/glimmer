// Codex — Claude-style hero layout (mirrors ch_claude.cpp with LILAC accent).
// v0.19: 24-h spark histogram at the bottom, driven by hourlyPct[] ring buffer.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>
#include <time.h>

// ── tick cache (position-based) ──
static float s_heroPct = -2.f;
static float s_secPct  = -2.f;
static float s_credits = -2.f;
static char  s_rightLine[16] = "";
static char  s_secSub[24] = "";
static int   s_sparkHour = -1;

bool chCodexEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showCodex && !ctx.settings->codexToken.isEmpty();
}

// Human label for a rate-limit window from its length in seconds.
static void windowLabel(long sec, char* buf, size_t n) {
    if      (sec >= 6L * 24 * 3600 - 3600) snprintf(buf, n, "WEEKLY");
    else if (sec >= 20L * 3600)            snprintf(buf, n, "DAILY");
    else if (sec >= 3600)                  snprintf(buf, n, "%ldH", sec / 3600);
    else if (sec > 0)                      snprintf(buf, n, "%ldM", sec / 60);
    else                                   buf[0] = '\0';
}

// Label for the secondary row: a per-model tag (e.g. "SPARK") if present,
// otherwise the window length.
static void secondaryLabel(const CodexData& d, long winSec, char* buf, size_t n) {
    if (d.secondaryTag[0]) { strncpy(buf, d.secondaryTag, n - 1); buf[n - 1] = '\0'; }
    else                     windowLabel(winSec, buf, n);
}

static void paintRightStack(const CodexData& d, time_t heroReset) {
    tft.fillRect(SCREEN_W - 110, 26, 100, 28, Theme::BG);
    if (d.creditsRemain >= 0) {
        char credits[16]; snprintf(credits, sizeof(credits), "$%.2f", d.creditsRemain);
        Display::useFont("VT323-32");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::SKY, Theme::BG);
        tft.drawString(credits, SCREEN_W - 12, 26);
        strncpy(s_rightLine, credits, sizeof(s_rightLine) - 1);
    } else if (heroReset > 0) {
        String r = Api::formatCountdown(heroReset);
        Display::useFont("VT323-32");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::LILAC, Theme::BG);
        tft.drawString(r, SCREEN_W - 12, 26);
        strncpy(s_rightLine, r.c_str(), sizeof(s_rightLine) - 1);
    } else {
        s_rightLine[0] = 0;
    }
}

static void paintPrimaryHero(float pct) {
    char pctBuf[8];
    if (pct < 0) snprintf(pctBuf, sizeof(pctBuf), "--");
    else         snprintf(pctBuf, sizeof(pctBuf), "%.0f", pct);
    tft.fillRect(10, 46, SCREEN_W - 110, 76, Theme::BG);

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

    Display::pixelBar(12, 128, SCREEN_W - 24, 8,
                     pct < 0 ? 0 : pct, uc);
}

static void paintSecondary(float pct, time_t secReset, const char* label) {
    tft.fillRect(0, 150, SCREEN_W, 20, Theme::BG);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(label, 12, 150);

    String right;
    if (secReset > 0) right = Api::formatCountdown(secReset);
    else if (pct >= 0) right = String((int)pct) + "%";
    else               right = "--";
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(right, SCREEN_W - 12, 150);
    strncpy(s_secSub, right.c_str(), sizeof(s_secSub) - 1);

    uint16_t uc = Display::usageColor(pct);
    Display::pixelBar(12, 164, SCREEN_W - 24, 4,
                     pct < 0 ? 0 : pct, uc);
}

static void paintSparkBar(const CodexData& d, int curHour) {
    const int sx = 12, sy = 188, sw = SCREEN_W - 24, sh = 31;
    const int barW = sw / 24;
    tft.fillRect(sx, 174, sw, 46, Theme::BG);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("24H", sx, 174);

    for (int i = 0; i < 24; i++) {
        int bx = sx + i * barW;
        if (!d.hourlyValid[i]) {
            tft.drawFastHLine(bx, sy + sh - 1, barW - 1, Theme::LINE);
            continue;
        }
        int bh = (d.hourlyPct[i] * sh) / 100;
        if (bh < 1) bh = 1;
        uint16_t c = (i == curHour) ? Theme::LILAC : Theme::INK_DIM;
        tft.fillRect(bx, sy + sh - bh, barW - 1, bh, c);
    }
    s_sparkHour = curHour;
}

void chCodexDraw(const ChannelCtx& ctx) {
    Display::clear();
    const char* cxModel = ctx.settings->codexModelLabel.length() > 0
                        ? ctx.settings->codexModelLabel.c_str() : "";
    Display::statusBar("Codex", cxModel, Theme::LILAC);

    const CodexData& d = *ctx.codex;
    if (d.err[0]) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString(d.err, SCREEN_W/2, 100);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Refresh token in web UI", SCREEN_W/2, 124);
        s_heroPct = -2.f; s_secPct = -2.f; s_credits = -2.f;
        s_rightLine[0] = 0;
        return;
    }
    if (!d.valid) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("No Codex data", SCREEN_W/2, 110);
        return;
    }

    // The weekly window is the primary now that Codex dropped the 5-hour cap.
    // Only swap hero/secondary when there are two *real* rate-limit windows; a
    // per-model additional limit (secondaryTag set) never takes the hero slot.
    const bool realSecondary = d.secondaryPct >= 0 && d.secondaryTag[0] == '\0';
    const bool swapped = realSecondary && ctx.settings->codexWeeklyHero;
    const float heroPct  = swapped ? d.secondaryPct    : d.primaryPct;
    const float secPct   = swapped ? d.primaryPct      : d.secondaryPct;
    const time_t heroRst = swapped ? d.secondaryReset  : d.primaryReset;
    const time_t secRst  = swapped ? d.primaryReset    : d.secondaryReset;
    const long  heroWin  = swapped ? d.secondaryWinSec : d.primaryWinSec;
    const long  secWin   = swapped ? d.primaryWinSec   : d.secondaryWinSec;

    char heroLbl[12]; windowLabel(heroWin, heroLbl, sizeof(heroLbl));
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(heroLbl, 12, 32);

    paintRightStack(d, heroRst);
    paintPrimaryHero(heroPct);

    Display::dotsDivider(12, 146, SCREEN_W - 24);

    char secLbl[16]; secondaryLabel(d, secWin, secLbl, sizeof(secLbl));
    paintSecondary(secPct, secRst, secLbl);

    Display::dotsDivider(12, 170, SCREEN_W - 24);

    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    paintSparkBar(d, tm.tm_hour);

    // Seed cache
    s_heroPct = (heroPct < 0) ? -2.f : heroPct;
    s_secPct  = (secPct  < 0) ? -2.f : secPct;
    s_credits = d.creditsRemain;
}

void chCodexTick(const ChannelCtx& ctx) {
    if (!ctx.codex) return;
    const CodexData& d = *ctx.codex;
    if (d.err[0] || !d.valid) return;

    const bool realSecondary = d.secondaryPct >= 0 && d.secondaryTag[0] == '\0';
    const bool swapped = realSecondary && ctx.settings->codexWeeklyHero;
    const float heroPct  = swapped ? d.secondaryPct    : d.primaryPct;
    const float secPct   = swapped ? d.primaryPct      : d.secondaryPct;
    const time_t heroRst = swapped ? d.secondaryReset  : d.primaryReset;
    const time_t secRst  = swapped ? d.primaryReset    : d.secondaryReset;
    const long  secWin   = swapped ? d.primaryWinSec   : d.secondaryWinSec;

    // Right stack — repaint when credits change OR countdown text changes
    bool rightDirty = false;
    if (d.creditsRemain >= 0) {
        if (fabsf(d.creditsRemain - s_credits) > 0.005f) rightDirty = true;
    } else if (heroRst > 0) {
        String r = Api::formatCountdown(heroRst);
        if (strcmp(r.c_str(), s_rightLine) != 0) rightDirty = true;
    }
    if (rightDirty) { paintRightStack(d, heroRst); s_credits = d.creditsRemain; }

    float p = (heroPct < 0) ? -2.f : heroPct;
    if (fabsf(p - s_heroPct) > 0.4f) {
        paintPrimaryHero(heroPct);
        s_heroPct = p;
    }

    // Secondary — repaint on pct change or countdown text change
    float sp = (secPct < 0) ? -2.f : secPct;
    bool secDirty = fabsf(sp - s_secPct) > 0.4f;
    if (!secDirty && secRst > 0) {
        String fresh = Api::formatCountdown(secRst);
        if (strcmp(fresh.c_str(), s_secSub) != 0) secDirty = true;
    }
    if (secDirty) {
        char secLbl[16]; secondaryLabel(d, secWin, secLbl, sizeof(secLbl));
        paintSecondary(secPct, secRst, secLbl);
        s_secPct = sp;
    }

    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    if (tm.tm_hour != s_sparkHour) {
        paintSparkBar(d, tm.tm_hour);
    }
}
