#pragma once
#include "esp_lcd_panel_ops.h"

// Show a modal overlay on DSI display with status text
// Renders a centered dark box with a simple message
void dsi_modal_show(esp_lcd_panel_handle_t panel, const char *title, const char *detail);

// Show download progress modal
void dsi_modal_progress(esp_lcd_panel_handle_t panel, const char *title,
                        int current, int total);

// Draw a lightweight banner at the bottom of the screen (no dimming)
// Suitable for calling every frame during camera preview
void dsi_banner(esp_lcd_panel_handle_t panel, const char *text);
