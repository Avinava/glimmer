#pragma once
// Shared status-bar chrome for the usage channels.
//
// Right-meta priority: STALE badge (our data is old) > vendor status badge
// (the provider says it is degraded) > the channel's normal meta.
#include <string.h>
#include "api.h"
#include "theme.h"
#include "vendor_status.h"

struct MetaSlot {
    char     text[16] = "";
    uint16_t color    = Theme::MUTED;
    bool operator!=(const MetaSlot& o) const { return color != o.color || strcmp(text, o.text); }
};

inline MetaSlot usageMeta(const Settings& s, time_t lastOk, VendorStatus::Vendor v,
                          const char* normal) {
    MetaSlot m;
    Api::staleText(lastOk, s, m.text, sizeof(m.text));
    if (m.text[0]) { m.color = Theme::AMBER; return m; }
    const char* b = s.showStatus ? VendorStatus::badge(v) : "";
    if (*b) {
        strncpy(m.text, b, sizeof(m.text) - 1);
        m.color = VendorStatus::badgeColor(v);
        return m;
    }
    strncpy(m.text, normal ? normal : "", sizeof(m.text) - 1);
    return m;
}

// Error-screen hint: a latched credential failure needs the user; anything
// else is retried in the background.
inline const char* errorHint(bool authErr) {
    return authErr ? "Update token in web UI" : "Offline - retrying";
}
