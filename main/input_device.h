#pragma once
#include <cstdint>

// Hardware input abstraction.
//
// Multiple backends (USB Xbox, GPIO buttons, I2C keypad, ...) register
// themselves as Sources at init time. Consumers call InputDev::get() to
// receive a single merged State. To add a new backend, fill in a static
// Source and call register_source() from your init function.
namespace InputDev {

enum Btn : uint32_t {
    DPAD_UP    = 1u << 0,
    DPAD_DOWN  = 1u << 1,
    DPAD_LEFT  = 1u << 2,
    DPAD_RIGHT = 1u << 3,
    A          = 1u << 4,
    B          = 1u << 5,
    X          = 1u << 6,
    Y          = 1u << 7,
    LB         = 1u << 8,   // "shoulderL" in Scratch input map
    RB         = 1u << 9,   // "shoulderR"
    LT         = 1u << 10,
    RT         = 1u << 11,
    START      = 1u << 12,
    BACK       = 1u << 13,
    LSTICK     = 1u << 14,  // stick click ("LeftStickPressed")
    RSTICK     = 1u << 15,  // ("RightStickPressed")
    GUIDE      = 1u << 16,
};

struct State {
    uint32_t buttons;        // OR of Btn flags
    int16_t  lx, ly, rx, ry; // Analog sticks, signed -32768..32767 (0 if unused)
    bool     any_connected;  // true if any registered source is live
};

// One backend. poll() should overwrite *out with this source's snapshot;
// the registry merges across sources (buttons OR'd, axes max-abs).
struct Source {
    const char *name;
    bool       (*connected)();           // is this source live?
    void       (*poll)(State *out);      // write current snapshot
    const char *(*status_msg)();         // optional, may be nullptr
};

void        register_source(const Source *src);
State       get();
bool        any_connected();
const char *status_msg();

} // namespace InputDev
