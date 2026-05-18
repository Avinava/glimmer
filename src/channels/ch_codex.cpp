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

// ── tick cache ──
static float s_primaryPct   = -2.f;
static float s_secondaryPct = -2.f;
static float s_credits      = -2.f;
static char  s_rightLine[16] = "";

bool chCodexEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showCodex && !ctx.settings->codexToken.isEmpty();
}

static void paintRightStack(const CodexData& d) {
    // Clear region spans y=28..66 to fully cover both the Silkscreen-12
    // value line (y=32, height ~14) AND the DMMono-11 subtitle (y=50,
    // height ~13 → bottom row at y=63). Previous 30px height was 5px
    // short on the subtitle, leaving residual when value strings shrank.
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
    } else if (d.primaryReset > 0) {
        String r = Api::formatCountdown(d.primaryReset);
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

    Display::useFont("VT323-86");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(pctBuf, 12, 46);
    int heroW = tft.textWidth(pctBuf);
    int heroH = tft.fontHeight();

    Display::useFont("VT323-32");
    tft.setTextColor(Theme::LILAC, Theme::BG);
    int pctY = 46 + (heroH - tft.fontHeight()) - 2;
    tft.drawString("%", 12 + heroW + 2, pctY);

    Display::pixelBar(12, 128, SCREEN_W - 24, 8,
                     pct < 0 ? 0 : pct, Theme::LILAC);
}

static void paintSecondary(float pct) {
    tft.fillRect(0, 150, SCREEN_W, 38, Theme::BG);
    if (pct >= 0) {
        char wp[6]; snprintf(wp, sizeof(wp), "%d%%", (int)pct);
        Display::useFont("VT323-32");
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::SKY, Theme::BG);
        tft.drawString(wp, SCREEN_W - 12, 152);
    }
    String wk = (pct < 0) ? String("--") : (String((int)pct) + "%");
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(wk, 12, 172);

    Display::pixelBar(12, 190, SCREEN_W - 24, 6,
                     pct < 0 ? 0 : pct, Theme::SKY);
}

void chCodexDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Codex", MoodId::NONE, "GPT-5", Theme::LILAC);

    const CodexData& d = *ctx.codex;
    if (d.err[0]) {
        Display::useFont("Silkscreen-16");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::CORAL, Theme::BG);
        tft.drawString(d.err, SCREEN_W/2, 100);
        Display::useFont("DMMono-11");
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString("Refresh token in web UI", SCREEN_W/2, 124);
        s_primaryPct = -2.f; s_secondaryPct = -2.f; s_credits = -2.f;
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

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("PRIMARY", 12, 32);

    paintRightStack(d);
    paintPrimaryHero(d.primaryPct);

    Display::dotsDivider(12, 146, SCREEN_W - 24);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("WEEKLY", 12, 156);

    paintSecondary(d.secondaryPct);

    // Seed cache
    s_primaryPct   = (d.primaryPct   < 0) ? -2.f : d.primaryPct;
    s_secondaryPct = (d.secondaryPct < 0) ? -2.f : d.secondaryPct;
    s_credits      = d.creditsRemain;
}

void chCodexTick(const ChannelCtx& ctx) {
    if (!ctx.codex) return;
    const CodexData& d = *ctx.codex;
    if (d.err[0] || !d.valid) return;

    // Right stack — repaint when credits change OR countdown text changes
    bool rightDirty = false;
    if (d.creditsRemain >= 0) {
        if (fabsf(d.creditsRemain - s_credits) > 0.005f) rightDirty = true;
    } else if (d.primaryReset > 0) {
        String r = Api::formatCountdown(d.primaryReset);
        if (strcmp(r.c_str(), s_rightLine) != 0) rightDirty = true;
    }
    if (rightDirty) { paintRightStack(d); s_credits = d.creditsRemain; }

    float p = (d.primaryPct < 0) ? -2.f : d.primaryPct;
    if (fabsf(p - s_primaryPct) > 0.4f) {
        paintPrimaryHero(d.primaryPct);
        s_primaryPct = p;
    }

    float sp = (d.secondaryPct < 0) ? -2.f : d.secondaryPct;
    if (fabsf(sp - s_secondaryPct) > 0.4f) {
        paintSecondary(d.secondaryPct);
        s_secondaryPct = sp;
    }
}
