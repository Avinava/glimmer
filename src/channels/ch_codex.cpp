// Codex — Claude-style hero layout (mirrors ch_claude.cpp with LILAC accent).
// Bottom: 24-h strip of the weekly allowance (hourly, from the usage history
// ring) with the weekly PACE on its label row.

#include "channel.h"
#include "chrome.h"
#include "display.h"
#include "history.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>
#include <time.h>

// ── tick cache (position-based) ──
static float s_heroPct = -2.f;
static float s_secPct  = -2.f;
static float s_credits = -2.f;
static bool  s_stale   = false;
static char  s_rightLine[16] = "";
static char  s_secSub[24] = "";
static char  s_pace[20] = "";
static char  s_resets[28] = "";
static Cred  s_cred = Cred::NOT_SET;
static uint32_t s_sparkHour = 0;
static float s_sparkPct = -2.f;     // primaryPct the strip's current bar shows
static MetaSlot s_meta;
static int   s_loadDot = -1;

static MetaSlot metaFor(const ChannelCtx& ctx) {
    return usageMeta(*ctx.settings, ctx.codex->lastOk, VendorStatus::OPENAI,
                     ctx.settings->codexModelLabel.c_str(), ctx.codex->cred, ctx.codex->jwtExp);
}

bool chCodexEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showCodex && !ctx.settings->codexToken.isEmpty();
}

// Human label for a rate-limit window from its length in seconds.
static void windowLabel(long sec, char* buf, size_t n) {
    if      (sec >= 6L * 24 * 3600 - 3600) snprintf_P(buf, n, PSTR("WEEKLY"));
    else if (sec >= 20L * 3600)            snprintf_P(buf, n, PSTR("DAILY"));
    else if (sec >= 3600)                  snprintf_P(buf, n, PSTR("%ldH"), sec / 3600);
    else if (sec > 0)                      snprintf_P(buf, n, PSTR("%ldM"), sec / 60);
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
        char credits[16]; snprintf_P(credits, sizeof(credits), PSTR("$%.2f"), d.creditsRemain);
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

static void paintPrimaryHero(float pct, bool stale) {
    char pctBuf[8];
    if (pct < 0) snprintf_P(pctBuf, sizeof(pctBuf), PSTR("--"));
    else         snprintf_P(pctBuf, sizeof(pctBuf), PSTR("%.0f"), pct);
    tft.fillRect(10, 46, SCREEN_W - 110, 76, Theme::BG);

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

static void paceFor(const CodexData& d, char* buf, size_t n) {
    History::Pace p = History::pace(UsageHistory::ring(), History::CODEX_WEEK,
                                    d.primaryPct, time(nullptr), d.primaryReset);
    History::paceText(p, buf, n);
}

// Bottom region y 174..219, in rows of 14 px:
//   [credential banner]   when the token is expired/rejected/blocked
//   24H + weekly pace     always
//   RESETS · 2 LEFT …     when the account has limit-reset credits (and no banner)
// then one bar per hour (oldest left, current hour right, in LILAC) = % of
// the weekly allowance remaining at that hour, filling what is left.
static void paintSparkBar(const CodexData& d, const char* pace) {
    const int sx = 12, sw = SCREEN_W - 24, bottom = 219;
    const int barW = sw / 24;
    tft.fillRect(0, 174, SCREEN_W, 46, Theme::BG);

    int y = 174;
    if (CredState::bad(d.cred)) { Chrome::credBanner(y, false, d.cred); y += 14; }

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("24H", sx, y);
    if (pace[0]) {
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(strncmp(pace, "EMPTY", 5) == 0 ? Theme::CORAL : Theme::INK_DIM, Theme::BG);
        tft.drawString(pace, SCREEN_W - 12, y);
    }
    y += 14;
    strncpy(s_pace, pace, sizeof(s_pace) - 1);

    char resets[28]; Chrome::resetsText(d.resets, resets, sizeof(resets));
    if (resets[0] && !CredState::bad(d.cred)) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::MINT, Theme::BG);
        tft.drawString("RESETS", sx, y);
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(resets, SCREEN_W - 12, y);
        y += 14;
    }
    strncpy(s_resets, resets, sizeof(s_resets) - 1);
    s_cred = d.cred;

    const int sh = bottom - y;
    uint32_t hourNow = (uint32_t)(time(nullptr) / 3600);
    const History::Ring& r = UsageHistory::ring();
    for (int i = 0; i < 24; i++) {
        int bx = sx + i * barW;
        int8_t v = History::at(r, hourNow - 23 + i, History::CODEX_WEEK);
        if (v < 0) {
            tft.drawFastHLine(bx, bottom, barW - 1, Theme::LINE);
            continue;
        }
        int bh = (v * sh) / 100;
        if (bh < 1) bh = 1;
        uint16_t c = (i == 23) ? Theme::LILAC : Theme::INK_DIM;
        tft.fillRect(bx, bottom + 1 - bh, barW - 1, bh, c);
    }
    s_sparkHour = hourNow;
}

void chCodexDraw(const ChannelCtx& ctx) {
    Display::clear();
    s_meta = metaFor(ctx);
    Display::statusBar("Codex", s_meta.text, Theme::LILAC, s_meta.color);

    const CodexData& d = *ctx.codex;
    // Nothing to show yet and something is wrong: the credential/offline card.
    if (!d.valid && (CredState::bad(d.cred) || d.err[0])) {
        Chrome::credCard(false, d.cred, d.jwtExp, d.err);
        s_heroPct = -2.f; s_secPct = -2.f; s_credits = -2.f;
        s_rightLine[0] = 0;
        return;
    }
    if (!d.valid) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Loading", SCREEN_W/2, 100);
        Display::loadingDots(SCREEN_W/2 - 21, 128, 0, Theme::LILAC);
        s_loadDot = 0;
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

    s_stale = dimData(*ctx.settings, d.lastOk, d.cred);
    paintRightStack(d, heroRst);
    paintPrimaryHero(heroPct, s_stale);

    Display::dotsDivider(12, 146, SCREEN_W - 24);

    char secLbl[16]; secondaryLabel(d, secWin, secLbl, sizeof(secLbl));
    paintSecondary(secPct, secRst, secLbl);

    Display::dotsDivider(12, 170, SCREEN_W - 24);

    char pace[20]; paceFor(d, pace, sizeof(pace));
    paintSparkBar(d, pace);
    s_sparkPct = d.primaryPct;

    // Seed cache
    s_heroPct = (heroPct < 0) ? -2.f : heroPct;
    s_secPct  = (secPct  < 0) ? -2.f : secPct;
    s_credits = d.creditsRemain;
}

void chCodexTick(const ChannelCtx& ctx) {
    if (!ctx.codex) return;
    const CodexData& d = *ctx.codex;
    if (!d.valid && (CredState::bad(d.cred) || d.err[0])) return;   // static card
    if (!d.valid) {                       // loading — sweep the chase dots
        int lit = (ctx.now_ms / 150) % 5;
        if (lit != s_loadDot) {
            Display::loadingDots(SCREEN_W/2 - 21, 128, lit, Theme::LILAC);
            s_loadDot = lit;
        }
        return;
    }

    // formatCountdown() has minute granularity, so only re-derive (and heap-
    // allocate) the countdown strings when the wall-clock minute rolls over.
    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    static int s_cdMin = -1;
    const bool minTick = (tm.tm_min != s_cdMin);

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
    } else if (minTick && heroRst > 0) {
        String r = Api::formatCountdown(heroRst);
        if (strcmp(r.c_str(), s_rightLine) != 0) rightDirty = true;
    }
    if (rightDirty) { paintRightStack(d, heroRst); s_credits = d.creditsRemain; }

    if (minTick) {
        MetaSlot m = metaFor(ctx);
        if (m != s_meta) { Display::statusMeta(m.text, Theme::LILAC, m.color); s_meta = m; }
    }

    float p = (heroPct < 0) ? -2.f : heroPct;
    bool stale = dimData(*ctx.settings, d.lastOk, d.cred);
    if (fabsf(p - s_heroPct) > 0.4f || stale != s_stale) {
        paintPrimaryHero(heroPct, stale);
        s_heroPct = p;
        s_stale = stale;
    }

    // Secondary — repaint on pct change or countdown text change
    float sp = (secPct < 0) ? -2.f : secPct;
    bool secDirty = fabsf(sp - s_secPct) > 0.4f;
    if (!secDirty && minTick && secRst > 0) {
        String fresh = Api::formatCountdown(secRst);
        if (strcmp(fresh.c_str(), s_secSub) != 0) secDirty = true;
    }
    if (secDirty) {
        char secLbl[16]; secondaryLabel(d, secWin, secLbl, sizeof(secLbl));
        paintSecondary(secPct, secRst, secLbl);
        s_secPct = sp;
    }

    // The strip shifts every hour; pace and resets move with each fetch.
    if (minTick || (uint32_t)(t / 3600) != s_sparkHour || d.cred != s_cred) {
        char pace[20]; paceFor(d, pace, sizeof(pace));
        char resets[28]; Chrome::resetsText(d.resets, resets, sizeof(resets));
        if ((uint32_t)(t / 3600) != s_sparkHour || strcmp(pace, s_pace) != 0
            || strcmp(resets, s_resets) != 0 || d.cred != s_cred
            || fabsf(d.primaryPct - s_sparkPct) > 0.4f) {
            paintSparkBar(d, pace);
            s_sparkPct = d.primaryPct;
        }
    }
    s_cdMin = tm.tm_min;
}
