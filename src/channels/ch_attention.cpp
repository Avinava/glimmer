// Attention — what agents (and the device) want the user to see now.
//
// Two channels share the queue in src/data/attention.{h,cpp}:
//
//   Attention  the interrupt card. Takes the screen when a card arrives (up to
//              30 s, or a legacy push's whole duration) and — with
//              pinApprovals — for as long as an agent is blocked on an
//              approval or a question. Several holders cycle every 6 s.
//
//      y 0..3     kind-colour strip; drains while the card has an expiry
//      y 12..28   [CLAUDE] tag + project
//      y ~56      title, Silkscreen-16, kind colour
//      y ~104     value, VT323-64 (when given)           — or the body here
//      y ~150     body, DMMono-11, up to 2 lines
//      y 168      progress bar (progress ≥ 0)
//      y 190      "waiting 2m · answer in your terminal" (approval / input)
//      y 210      "1 of 3"
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

static char     s_shownId[24] = "";
static uint32_t s_shownRev = 0;
static uint32_t s_cycleMs = 0;
static int      s_cycleIdx = 0;
static int      s_lastStrip = -1;
static uint32_t s_ageMin = 0xFFFFFFFF;

static void tagPill(int x, int y, const char* text, uint16_t color) {
    Display::useFont("DMMono-11");
    int w = tft.textWidth(text) + 8;
    tft.fillRect(x, y, w, 15, color);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(Theme::BG, color);
    tft.drawString(text, x + 4, y + 2);
}

// Up to 2 lines of DMMono-11, centred, word-wrapped at ~30 chars.
static void wrapCentered(const char* s, int y, uint16_t color) {
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(color, Theme::BG);
    const int maxW = SCREEN_W - 24;
    char line[41];
    snprintf(line, sizeof(line), "%s", s);
    if (tft.textWidth(line) <= maxW) { tft.drawString(line, SCREEN_W / 2, y); return; }
    // Break at the last space that fits.
    int cut = (int)strlen(line);
    while (cut > 0) {
        char c = line[cut]; line[cut] = '\0';
        bool fits = tft.textWidth(line) <= maxW;
        line[cut] = c;
        if (fits && (line[cut] == ' ' || line[cut] == '\0')) break;
        cut--;
    }
    if (cut <= 0) cut = 28;
    char a[41]; snprintf(a, sizeof(a), "%.*s", cut, line);
    tft.drawString(a, SCREEN_W / 2, y);
    const char* rest = line + cut; while (*rest == ' ') rest++;
    tft.drawString(rest, SCREEN_W / 2, y + 15);
}

static void paintStrip(const Item& it) {
    uint32_t now = AttentionQueue::now();
    int fill = SCREEN_W;
    if (it.expires && it.expires > it.created) {
        uint32_t total = it.expires - it.created;
        uint32_t left = it.expires > now ? it.expires - now : 0;
        fill = (int)((uint64_t)SCREEN_W * left / total);
    }
    if (fill == s_lastStrip) return;
    tft.fillRect(0, 0, SCREEN_W, 3, Theme::PANEL);
    tft.fillRect(0, 0, fill, 3, kindColor(it.kind));
    s_lastStrip = fill;
}

static void paintAge(const Item& it, int nHolders) {
    tft.fillRect(0, 182, SCREEN_W, 36, Theme::BG);
    Display::useFont("DMMono-11");
    tft.setTextDatum(TC_DATUM);
    uint32_t now = AttentionQueue::now();
    if (waitsOnUser(it.kind)) {
        char d[8]; TimeUtil::shortDuration((long)(now - it.created), d, sizeof(d));
        char line[40];
        snprintf_P(line, sizeof(line), PSTR("waiting %s \xC2\xB7 answer in terminal"), d);
        tft.setTextColor(Theme::INK_DIM, Theme::BG);
        tft.drawString(line, SCREEN_W / 2, 188);
    }
    if (nHolders > 1) {
        char f[16]; snprintf_P(f, sizeof(f), PSTR("%d of %d"), s_cycleIdx + 1, nHolders);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(f, SCREEN_W / 2, 206);
    }
    s_ageMin = (now - it.created) / 60;
}

static void paintCard(const Item& it, int nHolders) {
    tft.fillRect(0, 0, SCREEN_W, Layout::CONTENT_BOTTOM, Theme::BG);
    s_lastStrip = -1;
    paintStrip(it);

    const uint16_t kc = kindColor(it.kind);
    int x = 12;
    if (it.agent != AGENT_OTHER) {
        char tag[8];
        snprintf_P(tag, sizeof(tag), PSTR("%s"), it.agent == AGENT_CLAUDE ? "CLAUDE" : "CODEX");
        tagPill(x, 12, tag, agentColor(it.agent));
        x += tft.textWidth(tag) + 16;
    }
    if (it.project[0]) {
        Display::useFont("DMMono-11");
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(it.project, x, 14);
    }

    Display::useFont("Silkscreen-16");
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(kc, Theme::BG);
    tft.drawString(it.title[0] ? it.title : kindName(it.kind), SCREEN_W / 2, 58);

    int bodyY = 96;
    if (it.value[0]) {
        Display::useFont("VT323-64");
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        tft.drawString(it.value, SCREEN_W / 2, 108);
        bodyY = 146;
    }
    if (it.body[0]) wrapCentered(it.body, bodyY, Theme::INK_DIM);

    if (it.progress >= 0) Display::pixelBar(24, 170, SCREEN_W - 48, 8, it.progress, kc);

    paintAge(it, nHolders);
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

void chAttentionTick(const ChannelCtx& ctx) {
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
    paintStrip(it);
    if ((AttentionQueue::now() - it.created) / 60 != s_ageMin) paintAge(it, n);
}

// ── Agents list ─────────────────────────────────────────────────────────────

static uint32_t s_listRev = 0;
static uint32_t s_listMin = 0xFFFFFFFF;

bool chAgentsEnabled(const ChannelCtx&) {
    return !mainNightFace() && count(AttentionQueue::get()) > 0;
}

static void paintList() {
    const Queue& q = AttentionQueue::get();
    uint32_t now = AttentionQueue::now();
    int ord[kMax], n = ordered(q, ord);
    tft.fillRect(0, Layout::CONTENT_TOP, SCREEN_W, Layout::CONTENT_BOTTOM - Layout::CONTENT_TOP, Theme::BG);
    const int rowH = 38;
    for (int i = 0; i < n; i++) {
        const Item& it = q.items[ord[i]];
        int y = 30 + i * rowH;
        tft.fillRect(8, y, 3, rowH - 6, kindColor(it.kind));
        Display::useFont("DMMono-11");
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(it.agent == AGENT_OTHER ? Theme::MUTED : agentColor(it.agent), Theme::BG);
        tft.drawString(it.agent == AGENT_OTHER ? (it.project[0] ? it.project : "glimmer")
                                               : (it.agent == AGENT_CLAUDE ? "CLAUDE" : "CODEX"), 18, y);
        char age[8]; TimeUtil::shortDuration((long)(now - it.created), age, sizeof(age));
        tft.setTextDatum(TR_DATUM);
        tft.setTextColor(Theme::MUTED, Theme::BG);
        tft.drawString(age, SCREEN_W - 10, y);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(Theme::INK, Theme::BG);
        char line[44];
        if (it.body[0]) snprintf_P(line, sizeof(line), PSTR("%s \xC2\xB7 %s"), it.title, it.body);
        else            snprintf_P(line, sizeof(line), PSTR("%s"), it.title[0] ? it.title : kindName(it.kind));
        // Clip to the row width.
        while (strlen(line) > 4 && tft.textWidth(line) > SCREEN_W - 28) line[strlen(line) - 1] = '\0';
        tft.drawString(line, 18, y + 15);
        if (it.progress >= 0) Display::pixelBar(SCREEN_W - 70, y + 2, 34, 6, it.progress, kindColor(it.kind));
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
