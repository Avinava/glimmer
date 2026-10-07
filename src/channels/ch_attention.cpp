// Attention — what agents (and the device) want the user to see now.
//
// Two channels share the queue in src/data/attention.{h,cpp}:
//
//   Attention  the interrupt card. Takes the screen when a card arrives (up to
//              30 s, or a legacy push's whole duration) and — with
//              pinApprovals — for as long as an agent is blocked on an
//              approval or a question. Several holders cycle every 6 s.
//
//      (layouts below, next to paintCard)
//
//   Agents     the queue as a list (status bar "Agents" / "3 ACTIVE"), in
//              rotation while there is anything in it.

#include "channel.h"
#include "display.h"
#include "theme.h"
#include "config.h"
#include "layout.h"
#include "attention.h"
#include "timeutil.h"

using namespace Attention;

extern bool mainNightFace();
extern bool mainPinApprovals();
extern bool mainAgentNightShow();

uint16_t kindColor(Kind k) {
    switch (k) {
        case K_APPROVAL: return Theme::AMBER;
        case K_ERROR:    return Theme::CORAL;
        case K_INPUT:    return Theme::SKY;
        case K_PROGRESS: return Theme::LILAC;
        case K_WARNING:  return Theme::AMBER;
        case K_SUCCESS:  return Theme::MINT;
        default:         return Theme::INK;
    }
}

static uint16_t agentColor(Agent a) {
    return a == AGENT_CLAUDE ? Theme::CORAL : a == AGENT_CODEX ? Theme::LILAC : Theme::SKY;
}

// At night (night face up) only these get through: anything urgent, a user's
// red card (as before), and — if the user chose so — agents waiting on them.
static bool allowedAtNight(const Item& it) {
    if (it.urgent) return true;
    if (it.kind == K_ERROR && it.agent == AGENT_OTHER && strncmp(it.id, "sys:", 4)) return true;
    return mainAgentNightShow() && waitsOnUser(it.kind);
}

// Slots that currently hold the screen, in display order.
static int holders(int out[kMax]) {
    const Queue& q = AttentionQueue::get();
    uint32_t now = AttentionQueue::now();
    int ord[kMax], n = ordered(q, ord, now), m = 0;
    bool night = mainNightFace();
    for (int i = 0; i < n; i++) {
        const Item& it = q.items[ord[i]];
        if (!holdsScreen(it, now, mainPinApprovals())) continue;
        if (night && !allowedAtNight(it)) continue;
        out[m++] = ord[i];
    }
    return m;
}

bool chAttentionEnabled(const ChannelCtx&) {
    int h[kMax];
    return holders(h) > 0;
}

// ── card ────────────────────────────────────────────────────────────────────
//
// One focal element per card, centred, three type sizes at most:
//
//   y 44   ■ CLAUDE · glimmer          agent square + project, DMMono-11 muted
//   y 78   Bash / 142/142 / 64%        focal, VT323-64 (44 when it won't fit)
//   y 140  pio run -e nodemcuv2…       one detail line, DMMono-11 muted
//   y 176  NEEDS YOU · 2M              status, Silkscreen-12 in the kind colour
//   y 200  ● ○                         queue dots / time-left hairline
//
// (Result cards swap the last two: kind-coloured title at 146, detail at 166.)

static char     s_shownId[24] = "";
static uint32_t s_shownRev = 0;
static uint32_t s_cycleMs = 0;
static int      s_cycleIdx = 0;
static int      s_lastDrain = -1;
static char     s_status[24] = "";

static const char* agentName2(Agent a) {
    return a == AGENT_CLAUDE ? "CLAUDE" : a == AGENT_CODEX ? "CODEX" : nullptr;
}

// Truncate with an ellipsis until it fits maxW (current font).
static void fitLine(char* s, int maxW) {
    if (tft.textWidth(s) <= maxW) return;
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = '\0';
        char t[48]; snprintf(t, sizeof(t), "%s...", s);
        if (tft.textWidth(t) <= maxW) { snprintf(s, 48, "%s", t); return; }
    }
}

static void centerText(const char* font, const char* text, int y, uint16_t color, int maxW = 216) {
    Display::useFont(font);
    char buf[48]; snprintf(buf, sizeof(buf), "%s", text);
    fitLine(buf, maxW);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, Theme::BG);
    tft.drawString(buf, SCREEN_W / 2, y);
}

// "■ CLAUDE · glimmer" — square in the agent colour, words muted.
static void paintHeader(const Item& it) {
    char line[40];
    const char* a = agentName2(it.agent);
    if (a && it.project[0]) snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %s"), a, it.project);
    else if (a)             snprintf_P(line, sizeof(line), PSTR("%s"), a);
    else                    snprintf_P(line, sizeof(line), PSTR("%s"), it.project[0] ? it.project : "glimmer");
    Display::useFont("DMMono-11");
    int w = tft.textWidth(line) + (a ? 12 : 0);
    int x = (SCREEN_W - w) / 2;
    if (a) { tft.fillRect(x, 48, 6, 6, agentColor(it.agent)); x += 12; }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(line, x, 44);
}

// The biggest VT323 (64, else 44) that fits, centred at y.
static void focal(const char* text, int y) {
    Display::useFont("VT323-64");
    const char* f = tft.textWidth(text) <= 216 ? "VT323-64" : "VT323-44";
    Display::useFont(f);
    int dy = strcmp(f, "VT323-44") ? 0 : 8;
    centerText(f, text, y + dy, Theme::INK);
}

// "Bash: pio run" → tool "Bash", detail "pio run"; "wants to use Bash" → "Bash".
static void toolAndDetail(const char* body, char tool[24], char detail[44]) {
    tool[0] = detail[0] = '\0';
    const char* colon = strstr(body, ": ");
    if (colon && colon - body > 0 && colon - body < 20) {
        snprintf(tool, 24, "%.*s", (int)(colon - body), body);
        snprintf(detail, 44, "%s", colon + 2);
        return;
    }
    const char* use = strstr(body, "use ");
    if (use) { snprintf(tool, 24, "%s", use + 4); return; }
    snprintf(detail, 44, "%s", body);
}

static void statusText(const Item& it, char* buf, size_t n) {
    uint32_t secs = AttentionQueue::now() - it.created;
    char d[8];
    if (secs < 60) snprintf_P(d, sizeof(d), PSTR("%luS"), (unsigned long)secs);
    else           TimeUtil::shortDuration((long)secs, d, sizeof(d));
    snprintf_P(buf, n, PSTR("%s \xC2\xB7 %s"), it.kind == K_APPROVAL ? "NEEDS YOU" : "WAITING", d);
}

static void paintStatus(const Item& it) {
    char s[24]; statusText(it, s, sizeof(s));
    tft.fillRect(0, 172, SCREEN_W, 20, Theme::BG);
    centerText("Silkscreen-12", s, 176, kindColor(it.kind));
    snprintf(s_status, sizeof(s_status), "%s", s);
}

static void paintDots(int n, uint16_t color) {
    if (n < 2) return;
    int x = SCREEN_W / 2 - (n * 10 - 4) / 2;
    for (int i = 0; i < n; i++) tft.fillRect(x + i * 10, 200, 6, 6, i == s_cycleIdx ? color : Theme::LINE);
}

static void paintWaiting(const Item& it, int n) {
    const bool custom = it.title[0] && strcmp(it.title, "APPROVAL NEEDED") && strcmp(it.title, "WAITING FOR YOU");
    char tool[24], detail[44];
    toolAndDetail(it.body, tool, detail);
    if (it.kind == K_INPUT) {
        focal(custom ? it.title : "Your turn", 78);
        if (!detail[0] && tool[0]) snprintf(detail, sizeof(detail), "%s", it.body);
    } else {
        focal(custom ? it.title : (tool[0] ? tool : "Approve?"), 78);
    }
    if (detail[0]) centerText("DMMono-11", detail, 140, Theme::MUTED);
    paintStatus(it);
    paintDots(n, kindColor(it.kind));
}

// Hairline under result cards that shortens as the card's time runs out.
static void paintDrain(const Item& it) {
    if (!it.expires || it.expires <= it.created) return;
    uint32_t now = AttentionQueue::now();
    uint32_t total = it.expires - it.created, left = it.expires > now ? it.expires - now : 0;
    int w = (int)((uint64_t)60 * left / total);
    if (w == s_lastDrain) return;
    tft.drawFastHLine(90, 200, 60, Theme::LINE);
    if (w > 0) tft.drawFastHLine(90, 200, w, kindColor(it.kind));
    s_lastDrain = w;
}

static void upper(char* s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }

static void paintResult(const Item& it, int n) {
    const uint16_t kc = kindColor(it.kind);
    char title[28]; snprintf(title, sizeof(title), "%s", it.title[0] ? it.title : kindName(it.kind));
    upper(title);
    if (it.progress >= 0) {
        char pct[8]; snprintf_P(pct, sizeof(pct), PSTR("%d%%"), it.progress);
        focal(pct, 78);
        tft.fillRect(50, 146, 140, 4, Theme::PANEL);
        tft.fillRect(50, 146, 140 * it.progress / 100, 4, kc);
        centerText("Silkscreen-12", title, 164, kc);
        if (it.body[0]) centerText("DMMono-11", it.body, 184, Theme::MUTED);
    } else if (it.value[0]) {
        focal(it.value, 78);
        centerText("Silkscreen-12", title, 146, kc);
        if (it.body[0]) centerText("DMMono-11", it.body, 166, Theme::MUTED);
    } else {
        // No value: the title is the focal element, in the kind colour.
        Display::useFont("VT323-44");
        const char* f = tft.textWidth(it.title[0] ? it.title : kindName(it.kind)) <= 216 ? "VT323-44" : "VT323-32";
        centerText(f, it.title[0] ? it.title : kindName(it.kind), 92, kc);
        if (it.body[0]) centerText("DMMono-11", it.body, 146, Theme::MUTED);
    }
    s_lastDrain = -1;
    if (n > 1) paintDots(n, kc);
    else       paintDrain(it);
}

static void paintCard(const Item& it, int n) {
    tft.fillRect(0, 0, SCREEN_W, Layout::CONTENT_BOTTOM, Theme::BG);
    paintHeader(it);
    if (waitsOnUser(it.kind)) paintWaiting(it, n);
    else                      paintResult(it, n);
    snprintf(s_shownId, sizeof(s_shownId), "%s", it.id);
}

static int pickHolder(int h[kMax], int n) {
    if (s_cycleIdx >= n) s_cycleIdx = 0;
    return h[s_cycleIdx];
}

void chAttentionDraw(const ChannelCtx&) {
    Display::clear();
    int h[kMax], n = holders(h);
    if (!n) return;
    s_cycleIdx = 0;                         // newest arrival: start from the top
    s_cycleMs = millis();
    paintCard(AttentionQueue::get().items[pickHolder(h, n)], n);
    s_shownRev = AttentionQueue::revision();
}

void chAttentionTick(const ChannelCtx&) {
    int h[kMax], n = holders(h);
    if (!n) return;
    bool repaint = AttentionQueue::revision() != s_shownRev;
    if (n > 1 && millis() - s_cycleMs >= 6000) {
        s_cycleIdx = (s_cycleIdx + 1) % n;
        s_cycleMs = millis();
        repaint = true;
    }
    const Item& it = AttentionQueue::get().items[pickHolder(h, n)];
    if (repaint || strcmp(it.id, s_shownId) != 0) {
        paintCard(it, n);
        s_shownRev = AttentionQueue::revision();
        return;
    }
    if (waitsOnUser(it.kind)) {
        char s[24]; statusText(it, s, sizeof(s));
        if (strcmp(s, s_status)) paintStatus(it);
    } else if (n < 2 && it.progress < 0) {
        paintDrain(it);
    }
}

// ── Agents list ─────────────────────────────────────────────────────────────
//
//            AGENTS                   Silkscreen-12 muted, no status bar
//   ■ Needs you · Bash                DMMono-11 ink
//     claude · glimmer · 2m           DMMono-11 muted
//   ─────────────────────             hairline
//   (3 rows; a 4th+ becomes "+ N more")

static uint32_t s_listRev = 0;
static uint32_t s_listMin = 0xFFFFFFFF;

bool chAgentsEnabled(const ChannelCtx&) {
    return !mainNightFace() && countVisible(AttentionQueue::get(), AttentionQueue::now()) > 0;
}

static void listRow(const Item& it, int y, uint32_t now) {
    tft.fillRect(20, y + 4, 6, 6, kindColor(it.kind));
    char line[48];
    if (waitsOnUser(it.kind)) {
        char tool[24], detail[44];
        toolAndDetail(it.body, tool, detail);
        snprintf_P(line, sizeof(line), PSTR("%s%s%s"), it.kind == K_APPROVAL ? "Needs you" : "Your turn",
                   tool[0] ? " \xC2\xB7 " : "", tool);
    } else {
        char t[28]; snprintf(t, sizeof(t), "%s", it.title[0] ? it.title : kindName(it.kind));
        // Sentence case reads calmer than shouty caps in a list.
        for (char* p = t + 1; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        if (it.progress >= 0)  snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %d%%"), t, it.progress);
        else if (it.value[0])  snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %s"), t, it.value);
        else                   snprintf_P(line, sizeof(line), PSTR("%s"), t);
    }
    Display::useFont("DMMono-11");
    fitLine(line, 190);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(line, 36, y);

    char age[8]; TimeUtil::shortDuration((long)(now - it.created), age, sizeof(age));
    for (char* p = age; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    char meta[48];
    const char* a = it.agent == AGENT_CLAUDE ? "claude" : it.agent == AGENT_CODEX ? "codex" : nullptr;
    if (a && it.project[0]) snprintf_P(meta, sizeof(meta), PSTR("%s \xC2\xB7 %s \xC2\xB7 %s"), a, it.project, age);
    else if (a)             snprintf_P(meta, sizeof(meta), PSTR("%s \xC2\xB7 %s"), a, age);
    else if (it.project[0]) snprintf_P(meta, sizeof(meta), PSTR("%s \xC2\xB7 %s"), it.project, age);
    else                    snprintf_P(meta, sizeof(meta), PSTR("%s"), age);
    fitLine(meta, 190);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(meta, 36, y + 15);
}

static void paintList() {
    const Queue& q = AttentionQueue::get();
    uint32_t now = AttentionQueue::now();
    int ord[kMax], n = ordered(q, ord, now);
    tft.fillRect(0, 0, SCREEN_W, Layout::CONTENT_BOTTOM, Theme::BG);
    centerText("Silkscreen-12", "AGENTS", 10, Theme::MUTED);
    int shown = n > 3 ? 2 : n;
    for (int i = 0; i < shown; i++) {
        int y = 40 + i * 60;
        listRow(q.items[ord[i]], y, now);
        if (i < n - 1) tft.drawFastHLine(20, y + 44, 200, Theme::LINE);
    }
    if (n > shown) {
        char more[24]; snprintf_P(more, sizeof(more), PSTR("+ %d more"), n - shown);
        centerText("DMMono-11", more, 40 + shown * 60, Theme::MUTED);
    }
    s_listRev = AttentionQueue::revision();
    s_listMin = now / 60;
}

void chAgentsDraw(const ChannelCtx&) {
    Display::clear();
    paintList();
}

void chAgentsTick(const ChannelCtx&) {
    if (AttentionQueue::revision() != s_listRev || AttentionQueue::now() / 60 != s_listMin) paintList();
}
