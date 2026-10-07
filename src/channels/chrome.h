#pragma once
// Shared chrome for the usage channels (Claude, Codex) and the compact
// surfaces that summarise them (Home, AI): status-bar meta, credential cards,
// credential banners. One place so every screen says the same thing.
//
// Right-meta priority: credential problem (RE-AUTH / BLOCKED) > STALE badge >
// EXPIRES 2D > vendor status badge > the channel's normal meta.
#include <string.h>
#include "api.h"
#include "theme.h"
#include "vendor_status.h"

struct MetaSlot {
    char     text[16] = "";
    uint16_t color    = Theme::MUTED;
    bool operator!=(const MetaSlot& o) const { return color != o.color || strcmp(text, o.text); }
};

inline uint16_t credColor(Cred c) {
    return c == Cred::BLOCKED || c == Cred::EXPIRING ? Theme::AMBER : Theme::CORAL;
}

inline MetaSlot usageMeta(const Settings& s, time_t lastOk, VendorStatus::Vendor v,
                          const char* normal, Cred cred, time_t jwtExp) {
    MetaSlot m;
    time_t now = time(nullptr);
    if (CredState::bad(cred)) {
        CredState::meta(cred, jwtExp, now, m.text, sizeof(m.text));
        m.color = credColor(cred);
        return m;
    }
    Api::staleText(lastOk, s, m.text, sizeof(m.text));
    if (m.text[0]) { m.color = Theme::AMBER; return m; }
    if (cred == Cred::EXPIRING) {
        CredState::meta(cred, jwtExp, now, m.text, sizeof(m.text));
        m.color = Theme::AMBER;
        return m;
    }
    const char* b = s.showStatus ? VendorStatus::badge(v) : "";
    if (*b) {
        strncpy(m.text, b, sizeof(m.text) - 1);
        m.color = VendorStatus::badgeColor(v);
        return m;
    }
    strncpy(m.text, normal ? normal : "", sizeof(m.text) - 1);
    return m;
}

// Values on screen are not live: old data, or a credential that no longer works.
inline bool dimData(const Settings& s, time_t lastOk, Cred cred) {
    return CredState::bad(cred) || Api::isStale(lastOk, s);
}

namespace Chrome {
    // Full content-area card for a usage channel with no data to show:
    // a credential problem (key glyph / blocked glyph + what to do) or a
    // surfaced network error (err). Paints y 24..220.
    void credCard(bool isClaude, Cred cred, time_t jwtExp, const char* err);

    // 14-px banner over existing data: "KEY EXPIRED · glimmer.local".
    void credBanner(int y, bool isClaude, Cred cred);

    // Compact one-liner for Home/AI rows: "key expired · update",
    // "blocked · retrying", "add key · glimmer.local". "" when nothing to say.
    void credLine(bool isClaude, Cred cred, char* buf, size_t n);

    // Limit-reset credits: "2 LEFT · BY 12 OCT" (or "· 3D" inside a week);
    // "" when the account has none.
    void resetsText(const ResetGrant& g, char* buf, size_t n);
}
