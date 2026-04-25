#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// M5Stack Tab5 (ST7123, MIPI DSI, 720x1280 portrait panel, landscape use)
// DPI outputs 720x1280 portrait; content is rotated 90° CW for landscape viewing
#define DSI_LCD_W  720
#define DSI_LCD_H  1280

// Landscape logical dimensions (as seen by user holding Tab5 horizontally)
#define DSI_LANDSCAPE_W  1280
#define DSI_LANDSCAPE_H  720

// Scratch stage: 480x360 → fit to landscape height (720):
// scale = 720/360 = 2.0x → 960x720, centered horizontally
// Pillarbox: (1280-960)/2 = 160px left/right
#define DSI_SCRATCH_W  960
#define DSI_SCRATCH_H  720
#define DSI_SCRATCH_X_OFFSET  ((DSI_LANDSCAPE_W - DSI_SCRATCH_W) / 2)

// Backlight GPIO
#define DSI_BACKLIGHT_GPIO  22

// Initialize MIPI DSI display with ST7123 panel
// Returns the DPI panel handle, or nullptr on failure
esp_lcd_panel_handle_t dsi_display_init();

// Push Scratch framebuffer (480x360 RGB888) scaled to 720x540 centered on display
void dsi_display_update(esp_lcd_panel_handle_t panel,
                        const uint8_t *scratch_fb, int src_w, int src_h);
