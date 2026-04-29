#pragma once
#include "esp_lcd_panel_ops.h"

// Show a modal overlay on DSI display with status text
// Renders a centered white card with a blue title strip and optional detail line
void dsi_modal_show(esp_lcd_panel_handle_t panel, const char *title, const char *detail);

// Show download progress modal
void dsi_modal_progress(esp_lcd_panel_handle_t panel, const char *title,
                        int current, int total);

// Kid-friendly QR scanning overlay. Renders a solid Scratch-blue strip at
// the bottom of the landscape view (= portrait left edge) containing a
// big centered headline plus a smaller right-aligned hint. The strip is
// expected to be carved out of the camera preview by setting a matching
// `bottom_strip_h` via camera_set_preview_strips() — once drawn, the strip
// stays untouched by subsequent PPA SRM camera frames, so this function
// only needs to be called once per scan session (no per-frame work, no
// flicker).
//
// `bottom_strip_h` is in landscape pixels (matches camera_set_preview_strips).
void dsi_qr_overlay(esp_lcd_panel_handle_t panel,
                    const char *headline,
                    const char *hint,
                    int bottom_strip_h);

// Force the next dsi_qr_overlay() call to redraw even if the text matches
// the previous call. Use when something else has overwritten the strip
// region (e.g., a status modal) before re-entering scan.
void dsi_qr_overlay_invalidate();
