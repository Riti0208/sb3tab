#pragma once
#include <cstdint>

// Xbox controller button bits
#define XBOX_DPAD_UP     (1 << 0)
#define XBOX_DPAD_DOWN   (1 << 1)
#define XBOX_DPAD_LEFT   (1 << 2)
#define XBOX_DPAD_RIGHT  (1 << 3)
#define XBOX_START       (1 << 4)
#define XBOX_BACK        (1 << 5)
#define XBOX_LSTICK      (1 << 6)
#define XBOX_RSTICK      (1 << 7)
#define XBOX_LB          (1 << 8)
#define XBOX_RB          (1 << 9)
#define XBOX_GUIDE       (1 << 10)
#define XBOX_A           (1 << 12)
#define XBOX_B           (1 << 13)
#define XBOX_X           (1 << 14)
#define XBOX_Y           (1 << 15)

struct GamepadState {
    uint16_t buttons;       // Bitmask of XBOX_* flags
    uint8_t left_trigger;   // 0-255
    uint8_t right_trigger;  // 0-255
    int16_t lx, ly;         // Left stick
    int16_t rx, ry;         // Right stick
    bool connected;
};

// Initialize USB Host and start gamepad detection task.
// Call once at startup (after DSI/IO expander init for USB 5V power).
void gamepad_init();

// Get current gamepad state (lock-free, updated by background task).
GamepadState gamepad_get_state();

// Check if gamepad is connected.
bool gamepad_connected();

// Get last debug message (for on-screen display).
const char *gamepad_last_msg();
