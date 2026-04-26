#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// Initialize MIPI CSI camera (SC202CS, 1280x720 RGB565)
bool camera_init();

// Capture one frame and scan for QR code.
// If a QR code is found, its data is copied into qr_buf (null-terminated).
// If preview_panel is set, the camera frame is displayed on DSI.
// overlay_text is drawn at the bottom of the preview (or nullptr for none).
// Returns true if a QR code was detected.
bool camera_scan_qr(char *qr_buf, int qr_buf_size,
                    esp_lcd_panel_handle_t preview_panel,
                    const char *overlay_text = nullptr);

// Release camera resources
void camera_deinit();
