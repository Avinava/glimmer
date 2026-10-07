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
    int ord[kMax], n = ordered(q, ord), m = 0;
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
// Two layouts, both using the full 240×240 (status bar like every channel):
//
//  waiting (approval / input)            result (success / error / warning / info / progress)
//  [CLAUDE]  Approval      glimmer       [CODEX]   Done          api
//  ───────────────── amber               ───────────────── mint
//       NEEDS YOU      VT323-44                TESTS PASSED   Silkscreen-16
//  ▌BASH               panel                   142/142        VT323-86/64/44 (fits)
//  ▌$ pio run -e …     2 lines                 main · 3m      body (panel for error/warning)
//  WAITING      QUEUE                      ▬▬▬▬▬▬▬▬▬▬▬▬▬▬▬     time left, drains
//  2:14          1/2   VT323-32
//    answer in your terminal             progress: TESTS / 64% (VT323-86) / segmented bar

static char     s_shownId[24] = "";
static uint32_t s_shownRev = 0;
static uint32_t s_cycleMs = 0;
static int      s_cycleIdx = 0;
static int      s_lastDrain = -1;
static uint32_t s_lastSecs = 0xFFFFFFFF;

static const char* agentLabel(Agent a) {
    return a == AGENT_CLAUDE ? "CLAUDE" : a == AGENT_CODEX ? "CODEX" : nullptr;
}

static const char* barTitle(const Item& it) {
    switch (it.kind) {
        case K_APPROVAL: return "Approval";
        case K_INPUT:    return "Your turn";
        case K_ERROR:    return "Failed";
        case K_WARNING:  return "Heads up";
        case K_SUCCESS:  return "Done";
        case K_PROGRESS: return "Running";
        default:         return "Note";
    }
}

// Status bar + the agent name in its colour at the left.
static void paintBar(const Item& it) {
    Display::statusBar(barTitle(it), it.project, kindColor(it.kind));
    if (const char* a = agentLabel(it.agent)) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(agentColor(it.agent), Theme::BG);
        tft.drawString(a, 4, Layout::STATUS_BOTTOM / 2);
    }
}

// Word-wrap `s` into up to 2 lines that fit `maxW` (DMMono-11 loaded).
static int wrap2(const char* s, int maxW, char a[44], char b[44]) {
    a[0] = b[0] = '\0';
    char buf[48]; snprintf(buf, sizeof(buf), "%s", s);
    if (tft.textWidth(buf) <= maxW) { snprintf(a, 44, "%s", buf); return 1; }
    int len = (int)strlen(buf), cut = len;
    for (int i = len; i > 0; i--) {
        if (buf[i] != ' ' && buf[i] != '\0') continue;
        char c = buf[i]; buf[i] = '\0';
        bool fits = tft.textWidth(buf) <= maxW;
        buf[i] = c;
        if (fits) { cut = i; break; }
    }
    if (cut == len) {                                  // one long word: hard cut
        cut = len;
        while (cut > 1) { char c = buf[cut]; buf[cut] = '\0'; bool f = tft.textWidth(buf) <= maxW; buf[cut] = c; if (f) break; cut--; }
    }
    snprintf(a, 44, "%.*s", cut, buf);
    const char* rest = buf + cut; while (*rest == ' ') rest++;
    snprintf(b, 44, "%s", rest);
    while (strlen(b) > 1 && tft.textWidth(b) > maxW) b[strlen(b) - 1] = '\0';
    return b[0] ? 2 : 1;
}

// Inset panel: accent bar on the left, optional small caps label, 2 text lines.
static void panel(int y, int h, uint16_t accent, const char* label, const char* text) {
    tft.fillRect(10, y, 220, h, Theme::PANEL);
    tft.fillRect(10, y, 3, h, accent);
    int ty = y + 7;
    if (label && *label) {
        Display::useFont("Silkscreen-12");
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::PANEL);
        tft.drawString(label, 20, ty);
        ty += 17;
    }
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::PANEL);
    char a[44], b[44];
    wrap2(text, 204, a, b);
    tft.drawString(a, 20, ty);
    if (b[0]) tft.drawString(b, 20, ty + 15);
}

// "Bash: pio run" → label "BASH", text "$ pio run". Otherwise label = fallback.
static void splitTool(const char* body, const char* fallback, char label[16], char text[44]) {
    const char* colon = strstr(body, ": ");
    if (colon && colon - body > 0 && colon - body < 15) {
        int n = (int)(colon - body);
        for (int i = 0; i < n; i++) { char c = body[i]; label[i] = (c >= 'a' && c <= 'z') ? c - 32 : c; }
        label[n] = '\0';
        snprintf(text, 44, "$ %s", colon + 2);
        return;
    }
    snprintf(label, 16, "%s", fallback);
    snprintf(text, 44, "%s", body);
}

static void fmtWait(uint32_t secs, char* buf, size_t n) {
    if (secs < 3600) snprintf_P(buf, n, PSTR("%lu:%02lu"), (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    else             snprintf_P(buf, n, PSTR("%luh%02lu"), (unsigned long)(secs / 3600), (unsigned long)((secs % 3600) / 60));
}

static void paintWaitTimer(const Item& it) {
    uint32_t secs = AttentionQueue::now() - it.created;
    char t[12]; fmtWait(secs, t, sizeof(t));
    tft.fillRect(10, 166, 120, 30, Theme::BG);
    Display::useFont("VT323-32");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(t, 12, 166);
    s_lastSecs = secs;
}

static void paintWaiting(const Item& it, int n) {
    const uint16_t kc = kindColor(it.kind);
    Display::useFont("VT323-44");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(kc, Theme::BG);
    const bool custom = it.title[0] && strcmp(it.title, "APPROVAL NEEDED") && strcmp(it.title, "WAITING FOR YOU");
    tft.drawString(custom ? it.title : (it.kind == K_APPROVAL ? "NEEDS YOU" : "YOUR TURN"), SCREEN_W / 2, 30);

    char label[16], text[44];
    splitTool(it.body[0] ? it.body : (it.kind == K_APPROVAL ? "wants your approval" : "is waiting for you"),
              it.kind == K_APPROVAL ? "PERMISSION" : "WAITING FOR INPUT", label, text);
    panel(80, 62, kc, label, text);

    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString("WAITING", 12, 154);
    paintWaitTimer(it);
    if (n > 1) {
        tft.setTextDatum(TR_DATUM);
        tft.drawString("QUEUE", SCREEN_W - 12, 154);
        char q[12]; snprintf_P(q, sizeof(q), PSTR("%d/%d"), s_cycleIdx + 1, n);
        Display::useFont("VT323-32");
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString(q, SCREEN_W - 12, 166);
    }
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(Theme::INK_DIM, Theme::BG);
    tft.drawString("answer in your terminal", SCREEN_W / 2, 204);
}

// Time-left bar at the bottom of result cards; drains as the card expires.
static void paintDrain(const Item& it) {
    if (!it.expires || it.expires <= it.created) return;
    uint32_t now = AttentionQueue::now();
    uint32_t total = it.expires - it.created, left = it.expires > now ? it.expires - now : 0;
    int w = (int)((uint64_t)216 * left / total);
    if (w == s_lastDrain) return;
    tft.fillRect(12, 212, 216, 3, Theme::PANEL);
    tft.fillRect(12, 212, w, 3, kindColor(it.kind));
    s_lastDrain = w;
}

// Biggest VT323 that fits 216 px.
static void bigValue(const char* v, int y, uint16_t color) {
    static const char* kFonts[] = {"VT323-86", "VT323-64", "VT323-44"};
    for (const char* f : kFonts) {
        Display::useFont(f);
        if (tft.textWidth(v) <= 216 || f == kFonts[2]) {
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(color, Theme::BG);
            int h = tft.fontHeight();
            tft.drawString(v, SCREEN_W / 2, y + (70 - h) / 2);
            return;
        }
    }
}

static void paintProgress(const Item& it) {
    const uint16_t kc = kindColor(it.kind);
    Display::useFont("Silkscreen-16");
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(kc, Theme::BG);
    tft.drawString(it.title[0] ? it.title : "WORKING", 12, 34);
    char pct[6]; snprintf_P(pct, sizeof(pct), PSTR("%d"), it.progress);
    Display::useFont("VT323-86");
    tft.setTextColor(Theme::INK, Theme::BG);
    tft.drawString(pct, 12, 54);
    int w = tft.textWidth(pct), h = tft.fontHeight();
    Display::useFont("VT323-44");
    tft.setTextColor(kc, Theme::BG);
    tft.drawString("%", 12 + w + 2, 54 + h - tft.fontHeight() - 4);
    Display::pixelBar(12, 140, SCREEN_W - 24, 10, it.progress, kc);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TL_DATUM);
    if (it.body[0]) {
        char a[44], b[44];
        wrap2(it.body, 216, a, b);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(a, 12, 160);
        if (b[0]) tft.drawString(b, 12, 175);
    }
    char d[8]; TimeUtil::shortDuration((long)(AttentionQueue::now() - it.created), d, sizeof(d));
    char started[24]; snprintf_P(started, sizeof(started), PSTR("started %s ago"), d);
    for (char* p = started; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    tft.setTextColor(Theme::MUTED, Theme::BG);
    tft.drawString(started, 12, 196);
}

static void paintResult(const Item& it) {
    const uint16_t kc = kindColor(it.kind);
    const bool boxed = it.kind == K_ERROR || it.kind == K_WARNING;
    int y = it.value[0] ? 40 : 64;
    Display::useFont("Silkscreen-16");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(kc, Theme::BG);
    tft.drawString(it.title[0] ? it.title : kindName(it.kind), SCREEN_W / 2, y);
    y += 24;
    if (it.value[0]) { bigValue(it.value, y, Theme::INK); y += 80; }
    else             y += 10;
    if (it.body[0]) {
        if (boxed) panel(y, 42, kc, nullptr, it.body);
        else {
            Display::useFont("DMMono-11");
            char a[44], b[44];
            wrap2(it.body, 216, a, b);
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(Theme::INK_DIM, Theme::BG);
            tft.drawString(a, SCREEN_W / 2, y + 4);
            if (b[0]) tft.drawString(b, SCREEN_W / 2, y + 19);
        }
    }
    s_lastDrain = -1;
    paintDrain(it);
}

static void paintCard(const Item& it, int n) {
    tft.fillRect(0, 0, SCREEN_W, Layout::CONTENT_BOTTOM, Theme::BG);
    paintBar(it);
    if (waitsOnUser(it.kind))  paintWaiting(it, n);
    else if (it.progress >= 0) paintProgress(it);
    else                       paintResult(it);
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
        if (AttentionQueue::now() - it.created != s_lastSecs) paintWaitTimer(it);
    } else if (it.progress < 0) {
        paintDrain(it);
    }
}

// ── Agents list ─────────────────────────────────────────────────────────────
//
//  Agents                    3 ACTIVE
//  ▌[CLAUDE] glimmer              2m      44-px rows on PANEL,
//  ▌NEEDS YOU · Bash                      kind-colour bar on the left
//  ▌[CODEX] api                   4m
//  ▌TESTS          ▮▮▮▮▮▯▯▯▯

static uint32_t s_listRev = 0;
static uint32_t s_listMin = 0xFFFFFFFF;

bool chAgentsEnabled(const ChannelCtx&) {
    return !mainNightFace() && count(AttentionQueue::get()) > 0;
}

static void listRow(const Item& it, int y, uint32_t now) {
    const uint16_t kc = kindColor(it.kind);
    tft.fillRect(8, y, 224, 44, Theme::PANEL);
    tft.fillRect(8, y, 3, 44, kc);
    int x = 18;
    Display::useFont("DMMono-11");
    if (const char* a = agentLabel(it.agent)) {
        int w = tft.textWidth(a) + 8;
        tft.fillRect(x, y + 5, w, 15, agentColor(it.agent));
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::BG, agentColor(it.agent));
        tft.drawString(a, x + 4, y + 7);
        x += w + 6;
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::MUTED, Theme::PANEL);
    tft.drawString(it.project[0] ? it.project : (strncmp(it.id, "sys:", 4) ? "push" : "glimmer"), x, y + 7);
    char age[8]; TimeUtil::shortDuration((long)(now - it.created), age, sizeof(age));
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(waitsOnUser(it.kind) ? kc : Theme::MUTED, Theme::PANEL);
    tft.drawString(age, SCREEN_W - 14, y + 7);

    char line[48];
    const char* head = waitsOnUser(it.kind) ? (it.kind == K_APPROVAL ? "NEEDS YOU" : "YOUR TURN")
                                            : (it.title[0] ? it.title : kindName(it.kind));
    if (it.progress >= 0) {
        snprintf_P(line, sizeof(line), PSTR("%s"), head);
        Display::pixelBar(SCREEN_W - 112, y + 26, 98, 8, it.progress, kc);
    } else if (it.value[0]) snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %s"), head, it.value);
    else if (it.body[0])    snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %s"), head, it.body);
    else                    snprintf_P(line, sizeof(line), PSTR("%s"), head);
    int maxW = it.progress >= 0 ? SCREEN_W - 136 : SCREEN_W - 36;
    while (strlen(line) > 2 && tft.textWidth(line) > maxW) line[strlen(line) - 1] = '\0';
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::INK, Theme::PANEL);
    tft.drawString(line, 18, y + 25);
}

static void paintList() {
    const Queue& q = AttentionQueue::get();
    uint32_t now = AttentionQueue::now();
    int ord[kMax], n = ordered(q, ord);
    tft.fillRect(0, Layout::CONTENT_TOP, SCREEN_W, Layout::CONTENT_BOTTOM - Layout::CONTENT_TOP, Theme::BG);
    // 4 rows fit (44 px on a 48 px pitch from y 28); with 5, the last line
    // summarises the rest.
    int shown = n > 4 ? 3 : n;
    for (int i = 0; i < shown; i++) listRow(q.items[ord[i]], 28 + i * 48, now);
    if (n > shown) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        char more[24]; snprintf_P(more, sizeof(more), PSTR("+ %d more"), n - shown);
        tft.drawString(more, SCREEN_W / 2, 28 + shown * 48 + 14);
    }
    s_listRev = AttentionQueue::revision();
    s_listMin = now / 60;
}

void chAgentsDraw(const ChannelCtx&) {
    Display::clear();
    char meta[16];
    snprintf_P(meta, sizeof(meta), PSTR("%d ACTIVE"), count(AttentionQueue::get()));
    Display::statusBar("Agents", meta, Theme::SKY);
    paintList();
}

void chAgentsTick(const ChannelCtx&) {
    if (AttentionQueue::revision() != s_listRev) {
        char meta[16];
        snprintf_P(meta, sizeof(meta), PSTR("%d ACTIVE"), count(AttentionQueue::get()));
        Display::statusMeta(meta, Theme::SKY);
        paintList();
    } else if (AttentionQueue::now() / 60 != s_listMin) {
        paintList();
    }
}
