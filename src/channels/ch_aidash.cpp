// AI Dashboard — combined Claude+Codex glance.
//
// Layout:
//   - StatusBar "AI today" with total credits right
//   - Two big % side-by-side (CLAUDE coral, CODEX lilac)
//   - PixelBars stacked
//   - Reset countdown rows (real data from API)

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "api.h"
#include <math.h>
#include <time.h>

// tick cache
static float s_cl = -2.f, s_cx = -2.f;
static float s_credits = -2.f;
static char  s_clReset[12] = "";
static char  s_cxReset[12] = "";

bool chAiDashEnabled(const ChannelCtx& ctx) {
    return ctx.settings && ctx.settings->showAiDash
        && !ctx.settings->claudeKey.isEmpty()
        && !ctx.settings->codexToken.isEmpty();
}

static void paintCLBlock(float cl) {
    uint16_t uc = Display::usageColor(cl);
    tft.fillRect(10, 48, 100, 28, Theme::BG);
    char buf[8];
    if (cl < 0) snprintf(buf, sizeof(buf), "--%%");
    else        snprintf(buf, sizeof(buf), "%.0f%%", cl);
    Display::useFont("VT323-32");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(uc, Theme::BG);
    tft.drawString(buf, 12, 50);
    Display::pixelBar(12, 92, SCREEN_W - 24, 10, cl < 0 ? 0 : cl, uc);
}

static void paintCXBlock(float cx) {
    uint16_t uc = Display::usageColor(cx);
    tft.fillRect(SCREEN_W - 110, 48, 100, 28, Theme::BG);
    char buf[8];
    if (cx < 0) snprintf(buf, sizeof(buf), "--%%");
    else        snprintf(buf, sizeof(buf), "%.0f%%", cx);
    Display::useFont("VT323-32");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(uc, Theme::BG);
    tft.drawString(buf, SCREEN_W - 12, 50);
    Display::pixelBar(12, 110, SCREEN_W - 24, 10, cx < 0 ? 0 : cx, uc);
}

static void paintResetRow(int y, const char* tag, uint16_t tagColor, time_t resetEpoch) {
    tft.fillRect(0, y, SCREEN_W, 14, Theme::BG);
    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tagColor, Theme::BG);
    tft.drawString(tag, 12, y);

    String cd = Api::formatCountdown(resetEpoch);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString(cd, SCREEN_W - 12, y);
}

void chAiDashDraw(const ChannelCtx& ctx) {
    Display::clear();

    // Total spent right meta
    char rmeta[16] = "";
    const float credits = ctx.codex && ctx.codex->creditsRemain >= 0 ? ctx.codex->creditsRemain : -1;
    if (credits >= 0) snprintf(rmeta, sizeof(rmeta), "$%.2f", credits);
    Display::statusBar("AI today", rmeta, Theme::INK_DIM);

    const float cl = ctx.claude ? (ctx.settings->claudeWeeklyHero ? ctx.claude->weeklyPct : ctx.claude->sessionPct) : -1;
    const float cx = ctx.codex  ? Api::codexHeroPct(*ctx.settings, *ctx.codex) : -1;

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

    // Reset countdowns — real data from API
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("RESETS", 12, 138);

    time_t clReset = ctx.claude ? ctx.claude->sessionReset : 0;
    time_t cxReset = ctx.codex  ? ctx.codex->primaryReset  : 0;
    paintResetRow(158, "CL", Theme::CORAL, clReset);
    paintResetRow(174, "CX", Theme::LILAC, cxReset);

    // Seed cache
    s_cl = (cl < 0) ? -2.f : cl;
    s_cx = (cx < 0) ? -2.f : cx;
    s_credits = credits;
    strncpy(s_clReset, Api::formatCountdown(clReset).c_str(), sizeof(s_clReset) - 1);
    strncpy(s_cxReset, Api::formatCountdown(cxReset).c_str(), sizeof(s_cxReset) - 1);
}

void chAiDashTick(const ChannelCtx& ctx) {
    const float cl = ctx.claude ? (ctx.settings->claudeWeeklyHero ? ctx.claude->weeklyPct : ctx.claude->sessionPct) : -1.f;
    const float cx = ctx.codex  ? Api::codexHeroPct(*ctx.settings, *ctx.codex) : -1.f;
    float cl_eff = (cl < 0) ? -2.f : cl;
    float cx_eff = (cx < 0) ? -2.f : cx;

    if (fabsf(cl_eff - s_cl) > 0.4f) { paintCLBlock(cl); s_cl = cl_eff; }
    if (fabsf(cx_eff - s_cx) > 0.4f) { paintCXBlock(cx); s_cx = cx_eff; }

    // Credits — repaint just the right meta in the status bar
    const float credits = ctx.codex && ctx.codex->creditsRemain >= 0 ? ctx.codex->creditsRemain : -1.f;
    if (fabsf(credits - s_credits) > 0.005f) {
        char rmeta[16] = "";
        if (credits >= 0) snprintf(rmeta, sizeof(rmeta), "$%.2f", credits);
        tft.fillRect(SCREEN_W - 80, 0, 80, 21, Theme::BG);
        Display::useFont("DMMono-11");
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(rmeta, SCREEN_W - 4, 11);
        tft.drawFastHLine(SCREEN_W - 80, 21, 80, Theme::INK_DIM);
        s_credits = credits;
    }

    // Reset countdowns — text has minute granularity, so only recompute (and
    // heap-allocate) the strings when the wall-clock minute rolls over.
    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    static int s_cdMin = -1;
    if (tm.tm_min != s_cdMin) {
        s_cdMin = tm.tm_min;
        time_t clReset = ctx.claude ? ctx.claude->sessionReset : 0;
        time_t cxReset = ctx.codex  ? ctx.codex->primaryReset  : 0;
        String clFresh = Api::formatCountdown(clReset);
        String cxFresh = Api::formatCountdown(cxReset);
        if (strcmp(clFresh.c_str(), s_clReset) != 0) {
            paintResetRow(158, "CL", Theme::CORAL, clReset);
            strncpy(s_clReset, clFresh.c_str(), sizeof(s_clReset) - 1);
        }
        if (strcmp(cxFresh.c_str(), s_cxReset) != 0) {
            paintResetRow(174, "CX", Theme::LILAC, cxReset);
            strncpy(s_cxReset, cxFresh.c_str(), sizeof(s_cxReset) - 1);
        }
    }
}
