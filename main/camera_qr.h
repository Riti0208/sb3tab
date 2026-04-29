#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// Initialize MIPI CSI camera (SC202CS, 1280x720 RGB565)
bool camera_init();

// Configure the camera preview to carve out top/bottom strips (in landscape
// pixels) from the DPI framebuffer. PPA SRM will only write into the
// camera area, so a static overlay drawn in the strips by dsi_qr_overlay()
// is preserved across frames. Pass (0, 0) to use the full screen.
void camera_set_preview_strips(int top_h, int bottom_h);

// Capture one frame and scan for QR code.
// If a QR code is found, its data is copied into qr_buf (null-terminated).
// If preview_panel is set, the camera frame is displayed on DSI inside the
// non-strip area configured via camera_set_preview_strips().
// Returns true if a QR code was detected.
bool camera_scan_qr(char *qr_buf, int qr_buf_size,
                    esp_lcd_panel_handle_t preview_panel);

// Release camera resources
void camera_deinit();
