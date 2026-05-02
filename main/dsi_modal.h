#pragma once
#include "esp_lcd_panel_ops.h"

// Set a solid backdrop color (RGB565) for subsequent dsi_modal_show /
// dsi_modal_progress calls. Pass 0 to revert to the default "dim the
// previous framebuffer" backdrop. Used by the unified loading flow to
// keep the area outside the white card a consistent Scratch-blue across
// every progress update instead of fading toward black after the
// framebuffers are cleared.
void dsi_modal_set_bg(uint16_t rgb565);

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

// Switch-style chrome top bar drawn directly on DPI FB. Used during QR scan
// where LVGL is bypassed; with num_fbs=2 it must be redrawn each frame on
// the new back buffer. `batt_pct` < 0 hides the battery icon; otherwise a
// small battery rectangle is drawn manually (NotoSansJP doesn't ship the
// FontAwesome glyphs that LVGL uses, so we can't reuse the same icon char).
void dsi_qr_top_bar(esp_lcd_panel_handle_t panel,
                    const char *clock_text,
                    const char *batt_text,
                    int batt_pct,
                    bool batt_charging,
                    int top_strip_h);

void dsi_qr_top_bar_invalidate();
