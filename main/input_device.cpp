#include "input_device.h"
#include <cstdlib>

namespace InputDev {

constexpr int MAX_SOURCES = 4;
static const Source *s_sources[MAX_SOURCES] = {};
static int s_count = 0;

void register_source(const Source *src) {
    if (src && s_count < MAX_SOURCES) {
        s_sources[s_count++] = src;
    }
}

static inline int16_t pick_axis(int16_t a, int16_t b) {
    return (std::abs((int)b) > std::abs((int)a)) ? b : a;
}

State get() {
    State merged = {};
    for (int i = 0; i < s_count; ++i) {
        const Source *s = s_sources[i];
        if (!s || !s->poll) continue;
        State st = {};
        s->poll(&st);
        merged.buttons |= st.buttons;
        merged.lx = pick_axis(merged.lx, st.lx);
        merged.ly = pick_axis(merged.ly, st.ly);
        merged.rx = pick_axis(merged.rx, st.rx);
        merged.ry = pick_axis(merged.ry, st.ry);
        if (s->connected && s->connected()) merged.any_connected = true;
    }
    return merged;
}

bool any_connected() {
    for (int i = 0; i < s_count; ++i) {
        const Source *s = s_sources[i];
        if (s && s->connected && s->connected()) return true;
    }
    return false;
}

const char *status_msg() {
    for (int i = 0; i < s_count; ++i) {
        const Source *s = s_sources[i];
        if (s && s->status_msg) {
            const char *m = s->status_msg();
            if (m && *m) return m;
        }
    }
    return "";
}

} // namespace InputDev
