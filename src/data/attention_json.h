#pragma once
// JSON (POST /push body, MCP push_card arguments) → attention item.
// Pure (ArduinoJson only), host-tested — including the legacy push fields.
//
//   id        stable id; re-sending it updates the card in place (default: new card)
//   kind      approval | error | input | progress | warning | success | info
//   agent     claude | codex | other         (tag colour on the card)
//   title     ≤ 27 chars        value  big text, ≤ 15 chars       body  ≤ 39 chars
//   project   ≤ 15 chars (e.g. repo name)
//   progress  0..100 → progress bar (and "64%" as the value when none given)
//   ttl_s     seconds until it disappears; 0 = until cleared (max 24 h)
//   display   interrupt (default; takes the screen for up to 30 s) | queue
//             (only listed on the Agents channel)
//   urgent    may wake a dark screen at night
//
// Legacy fields still work: subtitle (= body), color (→ kind), duration_s
// (≤ 300 s; the card holds the screen for its whole duration, as before).
#include <ArduinoJson.h>
#include "attention_core.h"

namespace Attention {

inline void fromJson(JsonVariantConst d, uint32_t now, uint32_t approvalTtlS,
                     uint32_t autoSeq, Item& it) {
    it = Item();
    const char* id = d["id"] | "";
    if (*id) copyStr(it.id, sizeof(it.id), id);
    else     snprintf(it.id, sizeof(it.id), "push:%lu", (unsigned long)autoSeq);

    const char* kind = d["kind"] | "";
    it.kind  = *kind ? kindFrom(kind) : kindFromColor(d["color"] | "");
    it.agent = agentFrom(d["agent"] | "");
    copyStr(it.title,   sizeof(it.title),   d["title"] | "");
    copyStr(it.value,   sizeof(it.value),   d["value"] | "");
    const char* body = d["body"] | "";
    copyStr(it.body,    sizeof(it.body),    *body ? body : (d["subtitle"] | ""));
    copyStr(it.project, sizeof(it.project), d["project"] | "");
    it.urgent = d["urgent"] | false;

    if (!d["progress"].isNull()) {
        int p = d["progress"] | 0;
        it.progress = (int8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
        if (!it.value[0]) snprintf(it.value, sizeof(it.value), "%d%%", it.progress);
    }

    const char* disp = d["display"] | "";
    it.display = !strcmp(disp, "queue") ? SHOW_QUEUE
               : !strcmp(disp, "interrupt") ? SHOW_INTERRUPT
               : (it.kind == K_PROGRESS ? SHOW_QUEUE : SHOW_INTERRUPT);

    if (!d["ttl_s"].isNull()) {
        uint32_t ttl = d["ttl_s"] | 0UL;
        if (ttl > kMaxTtlS) ttl = kMaxTtlS;
        it.expires = ttl ? now + ttl : 0;
        it.interruptUntil = now + (ttl && ttl < kInterruptS ? ttl : kInterruptS);
    } else if (!d["duration_s"].isNull()) {           // legacy push
        uint32_t dur = d["duration_s"] | 30UL;
        if (dur > 300) dur = 300;
        if (dur < 1) dur = 1;
        it.expires = now + dur;
        it.interruptUntil = it.expires;
    } else {
        uint32_t ttl = defaultTtl(it.kind, approvalTtlS);
        it.expires = ttl ? now + ttl : 0;
        it.interruptUntil = now + (ttl && ttl < kInterruptS ? ttl : kInterruptS);
    }
}

inline void toJson(const Item& it, uint32_t now, JsonObject o) {
    o["id"]    = it.id;
    o["kind"]  = kindName(it.kind);
    o["agent"] = agentName(it.agent);
    o["title"] = it.title;
    if (it.value[0])   o["value"]   = it.value;
    if (it.body[0])    o["body"]    = it.body;
    if (it.project[0]) o["project"] = it.project;
    if (it.progress >= 0) o["progress"] = it.progress;
    o["display"] = it.display == SHOW_QUEUE ? "queue" : "interrupt";
    o["age_s"]   = now >= it.created ? now - it.created : 0;
    if (it.expires) o["expires_in_s"] = it.expires > now ? it.expires - now : 0;
    if (it.urgent) o["urgent"] = true;
    if (it.showAfter > now) o["shows_in_s"] = it.showAfter - now;
}

}  // namespace Attention
