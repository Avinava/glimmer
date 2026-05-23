// AI Dashboard — combined Claude+Codex glance.
//
// Design-true from AiDashScreen:
//   - StatusBar "AI today" with Pip THINKING + total credits right
//   - Two big % side-by-side (CLAUDE coral, CODEX lilac)
//   - PixelBars stacked
//   - 7-day mini chart (stub data until v0.11 history buffer lands)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include <math.h>

// tick cache
static float s_cl = -2.f, s_cx = -2.f;
static float s_credits = -2.f;

bool chAiDashEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showAiDash
        && !ctx.settings->claudeKey.isEmpty()
        && !ctx.settings->codexToken.isEmpty();
}

static void paintCLBlock(float cl) {
    // Hero %
    tft.fillRect(10, 48, 100, 28, Theme::BG);
    char buf[8];
    if (cl < 0) snprintf(buf, sizeof(buf), "--%%");
    else        snprintf(buf, sizeof(buf), "%.0f%%", cl);
    Display::useFont("VT323-32");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, 12, 50);
    // Bar
    Display::pixelBar(12, 92, SCREEN_W - 24, 10, cl < 0 ? 0 : cl, Theme::CORAL);
}

static void paintCXBlock(float cx) {
    tft.fillRect(SCREEN_W - 110, 48, 100, 28, Theme::BG);
    char buf[8];
    if (cx < 0) snprintf(buf, sizeof(buf), "--%%");
    else        snprintf(buf, sizeof(buf), "%.0f%%", cx);
    Display::useFont("VT323-32");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(buf, SCREEN_W - 12, 50);
    Display::pixelBar(12, 110, SCREEN_W - 24, 10, cx < 0 ? 0 : cx, Theme::LILAC);
}

void chAiDashDraw(const ChannelCtx& ctx) {
    Display::clear();

    // Total spent right meta
    char rmeta[16] = "";
    const float credits = ctx.codex && ctx.codex->creditsRemain >= 0 ? ctx.codex->creditsRemain : -1;
    if (credits >= 0) snprintf(rmeta, sizeof(rmeta), "$%.2f", credits);
    Display::statusBar("AI today", MoodId::NONE, rmeta, Theme::INK_DIM);

    const float cl = ctx.claude ? (ctx.settings->claudeWeeklyHero ? ctx.claude->weeklyPct : ctx.claude->sessionPct) : -1;
    const float cx = ctx.codex  ? (ctx.settings->codexWeeklyHero  ? ctx.codex->secondaryPct : ctx.codex->primaryPct) : -1;

    // ── Two big numbers side by side, design-true VLW typography ──
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::CORAL, Theme::BG);
    tft.drawString("CLAUDE", 12, 32);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::LILAC, Theme::BG);
    tft.drawString("CODEX", SCREEN_W - 12, 32);

    // Hero %s + segmented bars (via helpers so tick can reuse)
    paintCLBlock(cl);
    paintCXBlock(cx);

    Display::dotsDivider(12, 130, SCREEN_W - 24);

    // Section label — DMMono-11
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("LAST 7 DAYS", 12, 138);

    // 7-day stacked mini bars (v0.10 stub — replaces with real history in v0.11).
    // Pairs are (claude%, codex%). Today's bar uses live current values.
    int hist_cl[7] = {20, 38, 44, 30, 55, 62, (int)(cl < 0 ? 50 : cl)};
    int hist_cx[7] = {12, 22, 18, 34, 28, 31, (int)(cx < 0 ? 23 : cx)};
    int gap = 4;
    int barW = (SCREEN_W - 24 - gap * 6) / 7;
    int baseY = 200;
    const char dayLabel[] = "MTWTFSS";
    for (int i = 0; i < 7; i++) {
        int x = 12 + i * (barW + gap);
        int h1 = hist_cl[i] / 2;     // half-scale (max 50px-ish)
        int h2 = hist_cx[i] / 2;
        // Codex bar bottom, Claude on top — both grow upward from baseY
        tft.fillRect(x, baseY - h1, barW, h1, Theme::CORAL);
        tft.fillRect(x, baseY - h1 - h2 - 1, barW, h2, Theme::LILAC);
        // Day label below — DMMono-11
        char d[2] = { dayLabel[i], 0 };
        Display::useFont("DMMono-11");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(d, x + barW / 2, baseY + 6);
    }

    // Seed cache
    s_cl = (cl < 0) ? -2.f : cl;
    s_cx = (cx < 0) ? -2.f : cx;
    s_credits = credits;
}

void chAiDashTick(const ChannelCtx& ctx) {
    const float cl = ctx.claude ? (ctx.settings->claudeWeeklyHero ? ctx.claude->weeklyPct : ctx.claude->sessionPct) : -1.f;
    const float cx = ctx.codex  ? (ctx.settings->codexWeeklyHero  ? ctx.codex->secondaryPct : ctx.codex->primaryPct) : -1.f;
    float cl_eff = (cl < 0) ? -2.f : cl;
    float cx_eff = (cx < 0) ? -2.f : cx;

    if (fabsf(cl_eff - s_cl) > 0.4f) { paintCLBlock(cl); s_cl = cl_eff; }
    if (fabsf(cx_eff - s_cx) > 0.4f) { paintCXBlock(cx); s_cx = cx_eff; }

    // Credits — repaint just the right meta in the status bar
    const float credits = ctx.codex && ctx.codex->creditsRemain >= 0 ? ctx.codex->creditsRemain : -1.f;
    if (fabsf(credits - s_credits) > 0.005f) {
        // Just clear + redraw the status bar's right meta region.
        char rmeta[16] = "";
        if (credits >= 0) snprintf(rmeta, sizeof(rmeta), "$%.2f", credits);
        tft.fillRect(SCREEN_W - 80, 0, 80, 21, Theme::BG);
        Display::useFont("DMMono-11");
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(rmeta, SCREEN_W - 4, 11);
        // Re-stroke the underline since fillRect erased part of it
        tft.drawFastHLine(SCREEN_W - 80, 21, 80, Theme::INK_DIM);
        s_credits = credits;
    }
}
