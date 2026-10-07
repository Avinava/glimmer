#include "attention.h"
#include <Arduino.h>

namespace AttentionQueue {

static Attention::Queue s_q;
static uint32_t         s_rev = 1;
static bool             s_interrupt = false;

Attention::Queue& get() { return s_q; }
uint32_t revision()     { return s_rev; }

uint32_t now() {
    time_t t = time(nullptr);
    return t > 1000000000L ? (uint32_t)t : millis() / 1000;
}

int put(const Attention::Item& it, bool* fresh) {
    bool f = false;
    int i = Attention::upsert(s_q, it, now(), &f);
    if (i >= 0) {
        s_rev++;
        if (f && it.display == Attention::SHOW_INTERRUPT) s_interrupt = true;
    }
    if (fresh) *fresh = f;
    return i;
}

bool takeInterrupt() {
    bool v = s_interrupt;
    s_interrupt = false;
    return v;
}

bool clear(const char* id) {
    bool ok = Attention::clear(s_q, id);
    if (ok) s_rev++;
    return ok;
}

int clearAll() {
    int n = Attention::clearAll(s_q);
    if (n) s_rev++;
    return n;
}

int expire() {
    int n = Attention::expire(s_q, now());
    if (n) s_rev++;
    return n;
}

}  // namespace AttentionQueue
