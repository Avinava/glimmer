// Codex — Claude-style hero layout (mirrors ch_claude.cpp with LILAC accent).
//
// Note: the full Codex design (screens.jsx:239-290) wants a 24-h spark histogram
// driven by an in-RAM history buffer. That requires data plumbing not in v0.12 —
// for now we ship the symmetric Claude layout with primary % hero + weekly bar,
// using the existing CodexData fields.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include <math.h>

// ── tick cache (position-based) ──
static float s_heroPct = -2.f;
static float s_secPct  = -2.f;
static float s_credits = -2.f;
static char  s_rightLine[16] = "";

bool chCodexEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showCodex && !ctx.settings->codexToken.isEmpty();
}

static void paintRightStack(const CodexData& d, time_t heroReset) {
    tft.fillRect(SCREEN_W - 100, 28, 90, 38, Theme::BG);
    if (d.creditsRemain >= 0) {
        char credits[16]; snprintf(credits, sizeof(credits), "$%.2f", d.creditsRemain);
        Display::useFont("Silkscreen-12");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::SKY, Theme::BG);
        tft.drawString(credits, SCREEN_W - 12, 32);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("credits", SCREEN_W - 12, 50);
        strncpy(s_rightLine, credits, sizeof(s_rightLine) - 1);
    } else if (heroReset > 0) {
        String r = Api::formatCountdown(heroReset);
        Display::useFont("Silkscreen-12");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::LILAC, Theme::BG);
        tft.drawString(r, SCREEN_W - 12, 32);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("until reset", SCREEN_W - 12, 50);
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

    Display::useFont("VT323-32");
    tft.setTextColor(uc, Theme::BG);
    int pctY = 46 + (heroH - tft.fontHeight()) - 2;
    tft.drawString("%", 12 + heroW + 2, pctY);

    Display::pixelBar(12, 128, SCREEN_W - 24, 8,
                     pct < 0 ? 0 : pct, uc);
}

static void paintSecondary(float pct) {
    uint16_t uc = Display::usageColor(pct);
    tft.fillRect(0, 150, SCREEN_W, 38, Theme::BG);
    if (pct >= 0) {
        char wp[6]; snprintf(wp, sizeof(wp), "%d%%", (int)pct);
        Display::useFont("VT323-32");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(wp, SCREEN_W - 12, 152);
    }
    String wk = (pct < 0) ? String("--") : (String((int)pct) + "%");
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(wk, 12, 172);

    Display::pixelBar(12, 190, SCREEN_W - 24, 6,
                     pct < 0 ? 0 : pct, uc);
}

void chCodexDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Codex", "GPT-5", Theme::LILAC);

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

    const bool swapped = ctx.settings->codexWeeklyHero;
    const float heroPct  = swapped ? d.secondaryPct   : d.primaryPct;
    const float secPct   = swapped ? d.primaryPct     : d.secondaryPct;
    const time_t heroRst = swapped ? d.secondaryReset : d.primaryReset;

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(swapped ? "WEEKLY" : "PRIMARY", 12, 32);

    paintRightStack(d, heroRst);
    paintPrimaryHero(heroPct);

    Display::dotsDivider(12, 146, SCREEN_W - 24);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(swapped ? "PRIMARY" : "WEEKLY", 12, 156);

    paintSecondary(secPct);

    // Seed cache
    s_heroPct = (heroPct < 0) ? -2.f : heroPct;
    s_secPct  = (secPct  < 0) ? -2.f : secPct;
    s_credits = d.creditsRemain;
}

void chCodexTick(const ChannelCtx& ctx) {
    if (!ctx.codex) return;
    const CodexData& d = *ctx.codex;
    if (d.err[0] || !d.valid) return;

    const bool swapped = ctx.settings->codexWeeklyHero;
    const float heroPct  = swapped ? d.secondaryPct   : d.primaryPct;
    const float secPct   = swapped ? d.primaryPct     : d.secondaryPct;
    const time_t heroRst = swapped ? d.secondaryReset : d.primaryReset;

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

    float sp = (secPct < 0) ? -2.f : secPct;
    if (fabsf(sp - s_secPct) > 0.4f) {
        paintSecondary(secPct);
        s_secPct = sp;
    }
}
