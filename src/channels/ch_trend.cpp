// Trend — 7-day burn per provider, from the usage history ring.
//
//   y=0..22    StatusBar [Trend | 7-DAY]
//   per provider block (Claude, then Codex — whichever are configured):
//     header   tag left, "TODAY n · PEAK TUE" right          (DMMono-11)
//     bars     7 columns, oldest left, today right in the accent colour
//   y=168      weekday row
//   y=188      caption
//
// A bar = percentage points of that provider's weekly allowance used that day
// (positive drops summed; a window reset never reads as negative use).

#include "channel.h"
#include "display.h"
#include "history.h"
#include "chrome.h"
#include "theme.h"
#include "config.h"
#include <math.h>
#include <time.h>

static constexpr int kBlockY[2] = {30, 98};
static constexpr int kBarTop = 16, kBarH = 44;
static constexpr int kColW = 27, kColGap = 4, kX0 = 12;

static float    s_today[2] = {-1.f, -1.f};
static uint32_t s_hour = 0;
static Cred     s_cred[2] = {Cred::NOT_SET, Cred::NOT_SET};   // claude, codex at last paint

struct Block { const char* tag; uint16_t color; History::Metric metric; Cred cred; bool isClaude; };

static int blocksFor(const ChannelCtx& ctx, Block out[2]) {
    int n = 0;
    if (!ctx.settings->claudeKey.isEmpty())
        out[n++] = {"CLAUDE", Theme::CORAL, History::CLAUDE_WEEK, ctx.claude->cred, true};
    if (!ctx.settings->codexToken.isEmpty())
        out[n++] = {"CODEX",  Theme::LILAC, History::CODEX_WEEK,  ctx.codex->cred,  false};
    return n;
}

// Worth a slide only if some configured provider either has history to plot
// or a working credential that will produce some. All keys dead and nothing
// recorded = an empty chart; the Claude/Codex cards already explain why.
bool chTrendEnabled(const ChannelCtx& ctx) {
    if (!ctx.settings || !ctx.settings->showTrend || time(nullptr) < 1000000000L) return false;
    Block b[2];
    int n = blocksFor(ctx, b);
    for (int i = 0; i < n; i++) {
        if (!CredState::bad(b[i].cred)) return true;
        if (History::lastReading(UsageHistory::ring(), b[i].metric, time(nullptr))) return true;
    }
    return false;
}

static History::Trend trendFor(const Settings& s, History::Metric m) {
    return History::trend(UsageHistory::ring(), m, time(nullptr), (long)s.tzMinutes * 60L);
}

static void paintBlock(int idx, const Block& b, const History::Trend& t, float scale) {
    const int y = kBlockY[idx];
    tft.fillRect(0, y, SCREEN_W, kBarTop + kBarH + 2, Theme::BG);

    Display::useFont("Silkscreen-12");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(b.color, Theme::BG);
    tft.drawString(b.tag, kX0, y);

    // Header right: the day's numbers — or, when they can't be live, why.
    //   bad key + history:  "EXPIRED · LAST 3 AUG"  (coral/amber), bars dimmed
    //   bad key, no history: "token expired · update" (coral)
    //   no history yet:      "collecting data"        (muted)
    const bool bad = CredState::bad(b.cred);
    const time_t last = History::lastReading(UsageHistory::ring(), b.metric, time(nullptr));
    char right[40] = "";
    uint16_t rightColor = Theme::MUTED;
    char peak[4] = "";
    if (t.heaviest >= 0) {
        time_t d = time(nullptr) - (6 - t.heaviest) * 86400L;
        struct tm tm; localtime_r(&d, &tm);
        strftime(peak, sizeof(peak), "%a", &tm);
        for (char* p = peak; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    }
    if (bad && last) {
        struct tm tm; localtime_r(&last, &tm);
        char mon[4]; strftime(mon, sizeof(mon), "%b", &tm);
        for (char* p = mon; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        snprintf_P(right, sizeof(right), PSTR("%s \xC2\xB7 LAST %d %s"),
                   b.cred == Cred::BLOCKED ? "BLOCKED" : b.cred == Cred::EXPIRED ? "EXPIRED" : "REJECTED",
                   tm.tm_mday, mon);
        rightColor = credColor(b.cred);
    } else if (bad) {
        Chrome::credLine(b.isClaude, b.cred, right, sizeof(right));
        rightColor = credColor(b.cred);
    } else if (!last) {
        snprintf_P(right, sizeof(right), PSTR("collecting data"));
    } else if (t.have[6] && peak[0]) {
        snprintf_P(right, sizeof(right), PSTR("TODAY %.0f \xC2\xB7 PEAK %s"), t.burn[6], peak);
    } else if (t.have[6]) {
        snprintf_P(right, sizeof(right), PSTR("TODAY %.0f"), t.burn[6]);
    }
    Display::useFont("DMMono-11");
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(rightColor, Theme::BG);
    tft.drawString(right, SCREEN_W - 12, y + 1);

    const int base = y + kBarTop + kBarH;
    for (int i = 0; i < 7; i++) {
        int x = kX0 + i * (kColW + kColGap);
        tft.drawFastHLine(x, base, kColW, Theme::LINE);
        if (!t.have[i] || t.burn[i] <= 0) continue;
        int h = (int)(t.burn[i] / scale * kBarH + 0.5f);
        if (h < 2) h = 2;
        if (h > kBarH) h = kBarH;
        // Frozen data (dead key) is drawn dimmed, today included.
        uint16_t c = bad ? Theme::LINE : (i == 6 ? b.color : Theme::INK_DIM);
        tft.fillRect(x, base - h, kColW, h, c);
    }
}

static void paintWeekdays() {
    tft.fillRect(0, 166, SCREEN_W, 14, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    time_t now = time(nullptr);
    for (int i = 0; i < 7; i++) {
        time_t d = now - (6 - i) * 86400L;
        struct tm tm; localtime_r(&d, &tm);
        char wd[4]; strftime(wd, sizeof(wd), "%a", &tm);
        wd[2] = '\0';
        for (char* p = wd; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        tft.setTextColor(i == 6 ? Theme::INK : Theme::MUTED, Theme::BG);
        tft.drawString(wd, kX0 + i * (kColW + kColGap) + kColW / 2, 168);
    }
}

static void paintAll(const ChannelCtx& ctx) {
    Block b[2];
    int n = blocksFor(ctx, b);
    History::Trend t[2];
    float scale = 10.0f;                      // floor: tiny noise never fills a bar
    for (int i = 0; i < n; i++) {
        t[i] = trendFor(*ctx.settings, b[i].metric);
        for (float v : t[i].burn) if (v > scale) scale = v;
    }
    for (int i = 0; i < n; i++) {
        paintBlock(i, b[i], t[i], scale);
        s_today[i] = t[i].have[6] ? t[i].burn[6] : -1.f;
    }
    paintWeekdays();
    s_cred[0] = ctx.claude->cred; s_cred[1] = ctx.codex->cred;
    s_hour = (uint32_t)(time(nullptr) / 3600);
}

void chTrendDraw(const ChannelCtx& ctx) {
    Display::clear();
    Display::statusBar("Trend", "7-DAY", Theme::MINT);
    paintAll(ctx);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("weekly % used per day", SCREEN_W / 2, 194);
}

void chTrendTick(const ChannelCtx& ctx) {
    // Readings land every few minutes; re-derive once a minute and repaint
    // the bar blocks only when today's total moved or the hour rolled.
    static int s_min = -1;
    time_t now = time(nullptr);
    if (now / 60 == s_min) return;
    s_min = now / 60;
    Block b[2];
    int n = blocksFor(ctx, b);
    bool dirty = (uint32_t)(now / 3600) != s_hour;
    if (ctx.claude->cred != s_cred[0] || ctx.codex->cred != s_cred[1]) dirty = true;
    for (int i = 0; i < n && !dirty; i++) {
        History::Trend t = trendFor(*ctx.settings, b[i].metric);
        float today = t.have[6] ? t.burn[6] : -1.f;
        if (fabsf(today - s_today[i]) > 0.4f) dirty = true;
    }
    if (dirty) paintAll(ctx);
}
