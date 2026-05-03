#pragma once
#include <stdbool.h>
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize ST7123 touch controller (uses g_i2c_bus from dsi_display).
// Returns true on success. Spawns a polling task on core 0 (~66 Hz).
bool touch_input_init(void);

// Latest panel-coordinate reading (portrait, 0..719 x 0..1279).
typedef struct {
    int x;
    int y;
    bool pressed;
} touch_raw_t;

bool touch_input_get_raw(touch_raw_t *out);

// Latest reading mapped into Scratch stage coords (centered origin, Y up,
// approx ±240 / ±180 inside the pillarboxed area, may extend slightly
// outside on the side bars).
typedef struct {
    int stage_x;
    int stage_y;
    bool pressed;
} touch_stage_t;

bool touch_input_get_stage(touch_stage_t *out);

// Get the underlying esp_lcd_touch handle (for LVGL indev registration).
// Returns NULL if not initialized.
esp_lcd_touch_handle_t touch_input_get_handle(void);

#ifdef __cplusplus
}
#endif
