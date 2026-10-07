#pragma once
// Attention queue — what agents (and the device itself) want the user to see:
// approvals an agent is blocked on, questions, failures, progress, done.
// Pure (no Arduino) so the queue rules and the hook mapping are host-tested.
//
// Items are upserted by id: re-sending an id updates it in place (progress,
// text) without a new interruption. Kinds are ordered by priority; a full
// queue evicts its lowest-priority, oldest item, and never evicts an approval
// or question for something less important.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace Attention {

enum Kind : uint8_t {          // declaration order = priority (high → low)
    K_APPROVAL = 0,              // an agent is blocked on a permission prompt
    K_ERROR,                     // something failed
    K_INPUT,                     // an agent is waiting for an answer
    K_PROGRESS,                  // long-running work, 0..100 %
    K_WARNING,
    K_SUCCESS,
    K_INFO,
    KIND_COUNT
};

enum Agent : uint8_t { AGENT_OTHER = 0, AGENT_CLAUDE, AGENT_CODEX };
enum Display : uint8_t { SHOW_INTERRUPT = 0, SHOW_QUEUE };

constexpr int kMax = 5;
constexpr uint32_t kInterruptS = 30;      // how long a non-pinned card holds the screen
constexpr uint32_t kMaxTtlS    = 24UL * 3600UL;

struct Item {
    bool     used = false;
    char     id[24] = "";
    Kind     kind = K_INFO;
    Agent    agent = AGENT_OTHER;
    Display  display = SHOW_INTERRUPT;
    bool     urgent = false;           // may wake the panel at night
    int8_t   progress = -1;            // -1 = none
    char     title[28] = "";
    char     value[16] = "";           // optional big value ("passed", "64%")
    char     body[40] = "";
    char     project[16] = "";
    uint32_t created = 0;              // epoch
    uint32_t expires = 0;              // epoch, 0 = until cleared
    uint32_t interruptUntil = 0;       // epoch; card holds the screen until then
    uint32_t showAfter = 0;            // epoch; hidden until then (0 = at once)
};

// Hidden items are queued but not shown yet (e.g. Codex "your turn", which
// only surfaces if the user hasn't replied within a minute).
inline bool visible(const Item& it, uint32_t now) {
    return it.used && (!it.showAfter || now >= it.showAfter);
}

struct Queue { Item items[kMax]; };

// ── names ───────────────────────────────────────────────────────────────────

inline const char* kindName(Kind k) {
    static const char* n[KIND_COUNT] = {"approval", "error", "input", "progress", "warning", "success", "info"};
    return k < KIND_COUNT ? n[k] : "info";
}
inline Kind kindFrom(const char* s, Kind def = K_INFO) {
    if (!s || !*s) return def;
    for (int i = 0; i < KIND_COUNT; i++) if (!strcmp(s, kindName((Kind)i))) return (Kind)i;
    return def;
}
// Legacy push colours → the kind whose colour they were.
inline Kind kindFromColor(const char* c) {
    if (!c || !*c) return K_INFO;
    if (!strcmp(c, "red") || !strcmp(c, "coral") || !strcmp(c, "alert")) return K_ERROR;
    if (!strcmp(c, "amber") || !strcmp(c, "warn"))                        return K_WARNING;
    if (!strcmp(c, "green") || !strcmp(c, "mint") || !strcmp(c, "ok"))    return K_SUCCESS;
    if (!strcmp(c, "lilac") || !strcmp(c, "codex"))                       return K_PROGRESS;
    return K_INFO;   // sky / blue / info / unknown
}
inline const char* agentName(Agent a) {
    return a == AGENT_CLAUDE ? "claude" : a == AGENT_CODEX ? "codex" : "other";
}
inline Agent agentFrom(const char* s) {
    if (!s) return AGENT_OTHER;
    if (!strcmp(s, "claude")) return AGENT_CLAUDE;
    if (!strcmp(s, "codex"))  return AGENT_CODEX;
    return AGENT_OTHER;
}
inline bool waitsOnUser(Kind k) { return k == K_APPROVAL || k == K_INPUT; }

// Default lifetime (s) for a kind when the sender gives none. 0 = until cleared.
inline uint32_t defaultTtl(Kind k, uint32_t approvalTtlS) {
    switch (k) {
        case K_APPROVAL: case K_INPUT: return approvalTtlS;
        case K_ERROR:    return 300;
        case K_WARNING:  return 60;
        case K_PROGRESS: return 3600;
        default:       return 30;
    }
}

inline void copyStr(char* dst, size_t n, const char* src) {
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
}

// ── queue operations ────────────────────────────────────────────────────────

inline int find(const Queue& q, const char* id) {
    for (int i = 0; i < kMax; i++) if (q.items[i].used && !strcmp(q.items[i].id, id)) return i;
    return -1;
}

// a before b in display order: higher priority first; among equals, the one
// waiting longest first (FIFO for approvals).
inline bool before(const Item& a, const Item& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.created < b.created;
}

// Insert or update. Returns the slot, or -1 when the queue is full of things
// at least as important. *fresh = it was not already there (callers interrupt
// only for fresh items or a kind that became more urgent).
inline int upsert(Queue& q, const Item& in, uint32_t now, bool* fresh = nullptr) {
    int i = find(q, in.id);
    if (fresh) *fresh = (i < 0);
    if (i >= 0) {
        Item& it = q.items[i];
        bool escalated = in.kind < it.kind;
        uint32_t created = it.created, until = it.interruptUntil;
        it = in;
        it.used = true;
        it.created = escalated ? now : created;
        it.interruptUntil = escalated ? in.interruptUntil : until;
        if (fresh && escalated) *fresh = true;
        return i;
    }
    for (int k = 0; k < kMax; k++) if (!q.items[k].used) { i = k; break; }
    if (i < 0) {
        // Evict the least important, oldest — only if it is less important
        // than (or as important as, but older than) the newcomer, and never an
        // item the user still has to act on for something that isn't one.
        int victim = -1;
        for (int k = 0; k < kMax; k++) {
            const Item& c = q.items[k];
            if (victim < 0 || c.kind > q.items[victim].kind ||
                (c.kind == q.items[victim].kind && c.created < q.items[victim].created)) victim = k;
        }
        const Item& v = q.items[victim];
        if (v.kind < in.kind) return -1;
        if (waitsOnUser(v.kind) && !waitsOnUser(in.kind)) return -1;
        i = victim;
    }
    q.items[i] = in;
    q.items[i].used = true;
    q.items[i].created = now;
    return i;
}

inline bool clear(Queue& q, const char* id) {
    int i = find(q, id);
    if (i < 0) return false;
    q.items[i] = Item();
    return true;
}

inline int clearAll(Queue& q) {
    int n = 0;
    for (auto& it : q.items) if (it.used) { it = Item(); n++; }
    return n;
}

// Remove expired items; returns how many went.
inline int expire(Queue& q, uint32_t now) {
    int n = 0;
    for (auto& it : q.items) if (it.used && it.expires && now >= it.expires) { it = Item(); n++; }
    return n;
}

inline int count(const Queue& q) {
    int n = 0;
    for (const auto& it : q.items) n += it.used;
    return n;
}
inline int countKind(const Queue& q, Kind k) {
    int n = 0;
    for (const auto& it : q.items) n += (it.used && it.kind == k);
    return n;
}
inline int countWaiting(const Queue& q, uint32_t now) {
    int n = 0;
    for (const auto& it : q.items) n += (visible(it, now) && waitsOnUser(it.kind));
    return n;
}
inline int countVisible(const Queue& q, uint32_t now) {
    int n = 0;
    for (const auto& it : q.items) n += visible(it, now);
    return n;
}

// Slots in display order; returns how many. With now != 0, hidden items
// (showAfter in the future) are left out.
inline int ordered(const Queue& q, int out[kMax], uint32_t now = 0) {
    int n = 0;
    for (int i = 0; i < kMax; i++)
        if (q.items[i].used && (!now || visible(q.items[i], now))) out[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && before(q.items[out[b]], q.items[out[b - 1]]); b--) {
            int t = out[b]; out[b] = out[b - 1]; out[b - 1] = t;
        }
    return n;
}

// Should the attention card hold the screen? pinWaiting = settings.pinApprovals.
inline bool holdsScreen(const Item& it, uint32_t now, bool pinWaiting) {
    if (!visible(it, now)) return false;
    if (pinWaiting && waitsOnUser(it.kind)) return true;
    return it.display == SHOW_INTERRUPT && now < it.interruptUntil;
}

// ── agent hook events ───────────────────────────────────────────────────────
//
// The trimmed event an agent hook script sends (tools/agents/glimmer-hook.sh):
// Claude Code and Codex share event names (Notification, PermissionRequest,
// PostToolUse, UserPromptSubmit, Stop, SessionEnd, …).
struct HookEvent {
    const char* event   = "";   // hook_event_name
    const char* type    = "";   // notification_type (Claude Notification)
    const char* session = "";   // session_id
    const char* cwd     = "";
    const char* tool    = "";   // tool_name
    const char* detail  = "";   // short tool input (command / file path)
    const char* message = "";   // notification message
};

enum HookAction : uint8_t { HOOK_NONE, HOOK_UPSERT, HOOK_CLEAR };

// Codex has no "waiting for input" event; its Stop (turn finished, control
// back to the user) becomes a "your turn" card that stays hidden this long,
// so a quick reply (UserPromptSubmit clears it) never shows anything —
// the same grace Claude Code gives its own idle_prompt notification.
constexpr uint32_t kCodexIdleS = 60;

// One card per agent session: "<agent>:<first 8 of session>".
inline void sessionId(Agent a, const char* session, char* out, size_t n) {
    char s[9] = "";
    copyStr(s, sizeof(s), session && *session ? session : "default");
    snprintf(out, n, "%s:%s", agentName(a), s);
}

inline void basename(const char* path, char* out, size_t n) {
    const char* b = path ? path : "";
    size_t len = strlen(b);
    while (len > 1 && b[len - 1] == '/') len--;           // trailing slash
    const char* p = b + len;
    while (p > b && p[-1] != '/') p--;
    size_t m = (size_t)((b + len) - p);
    if (m >= n) m = n - 1;
    memcpy(out, p, m);
    out[m] = '\0';
}

// Map one hook event to an action on the queue. On HOOK_UPSERT `out` is the
// item; on HOOK_CLEAR `out.id` is the card to clear. `extra`, when non-null
// and its `used` comes back true, is a second card to upsert (Codex Stop
// with done cards on: the DONE card next to the delayed "your turn").
inline HookAction fromHook(Agent agent, const HookEvent& e, bool doneCards,
                           uint32_t approvalTtlS, uint32_t now, Item& out,
                           Item* extra = nullptr) {
    out = Item();
    if (extra) *extra = Item();
    sessionId(agent, e.session, out.id, sizeof(out.id));
    out.agent = agent;
    basename(e.cwd, out.project, sizeof(out.project));
    const char* ev = e.event ? e.event : "";
    const char* ty = e.type ? e.type : "";

    bool approval = !strcmp(ev, "PermissionRequest")
                 || (!strcmp(ev, "Notification") && !strcmp(ty, "permission_prompt"));
    bool input    = !strcmp(ev, "Notification")
                 && (!strcmp(ty, "idle_prompt") || !strcmp(ty, "agent_needs_input")
                     || !strcmp(ty, "elicitation_dialog"));
    if (approval || input) {
        out.kind = approval ? K_APPROVAL : K_INPUT;
        copyStr(out.title, sizeof(out.title), approval ? "APPROVAL NEEDED" : "WAITING FOR YOU");
        if (e.detail && *e.detail) {
            if (e.tool && *e.tool) snprintf(out.body, sizeof(out.body), "%s: %s", e.tool, e.detail);
            else                   copyStr(out.body, sizeof(out.body), e.detail);
        } else if (e.message && *e.message) {
            // "Claude needs your permission to use Bash" → "wants to use Bash"
            const char* use = strstr(e.message, "permission to use ");
            if (use) snprintf(out.body, sizeof(out.body), "wants to use %s", use + 18);
            else     copyStr(out.body, sizeof(out.body), e.message);
        } else if (e.tool && *e.tool) {
            copyStr(out.body, sizeof(out.body), e.tool);
        }
        out.display = SHOW_INTERRUPT;
        out.expires = approvalTtlS ? now + approvalTtlS : 0;
        out.interruptUntil = now + kInterruptS;
        return HOOK_UPSERT;
    }

    bool finished = !strcmp(ev, "Stop")
                 || (!strcmp(ev, "Notification") && !strcmp(ty, "agent_completed"));
    if (agent == AGENT_CODEX && !strcmp(ev, "Stop")) {
        if (doneCards && extra) {
            Item& d = *extra;
            d = out;
            snprintf(d.id, sizeof(d.id), "%.18s:done", out.id);
            d.kind = K_SUCCESS;
            copyStr(d.title, sizeof(d.title), "DONE");
            copyStr(d.body, sizeof(d.body), "Codex finished");
            d.display = SHOW_INTERRUPT;
            d.expires = now + 20;
            d.interruptUntil = now + 20;
            d.used = true;
        }
        out.kind = K_INPUT;
        copyStr(out.title, sizeof(out.title), "WAITING FOR YOU");
        copyStr(out.body, sizeof(out.body), e.message);
        out.display = SHOW_INTERRUPT;
        out.showAfter = now + kCodexIdleS;
        out.expires = approvalTtlS ? out.showAfter + approvalTtlS : 0;
        out.interruptUntil = out.showAfter + kInterruptS;
        return HOOK_UPSERT;
    }
    if (finished && doneCards) {
        out.kind = K_SUCCESS;
        copyStr(out.title, sizeof(out.title), "DONE");
        snprintf(out.body, sizeof(out.body), "%s finished", agent == AGENT_CODEX ? "Codex" : "Claude");
        out.display = SHOW_INTERRUPT;
        out.expires = now + 20;
        out.interruptUntil = now + 20;
        return HOOK_UPSERT;      // replaces the session's approval/input card
    }

    if (finished || !strcmp(ev, "PostToolUse") || !strcmp(ev, "PostToolUseFailure")
        || !strcmp(ev, "UserPromptSubmit") || !strcmp(ev, "SessionEnd"))
        return HOOK_CLEAR;
    return HOOK_NONE;
}

}  // namespace Attention
