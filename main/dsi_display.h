#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// Waveshare 43H-800480-IPS-CT (EK79007, MIPI DSI)
#define DSI_LCD_W  800
#define DSI_LCD_H  480

// Scratch stage rendered area (scaled to fit display height)
// 480x360 * (4/3) = 640x480, centered with 80px letterbox on each side
#define DSI_SCRATCH_W  640
#define DSI_SCRATCH_H  480
#define DSI_SCRATCH_X_OFFSET  ((DSI_LCD_W - DSI_SCRATCH_W) / 2)

// Initialize MIPI DSI display with EK79007 panel + PPA clients
// Returns the DPI panel handle, or nullptr on failure
esp_lcd_panel_handle_t dsi_display_init();

// Scale the Scratch framebuffer (480x360 RGB888) to 640x480 and draw centered on display
void dsi_display_update(esp_lcd_panel_handle_t panel,
                        const uint8_t *scratch_fb, int src_w, int src_h);
