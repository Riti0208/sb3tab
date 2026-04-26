// Simple modal overlay for DSI display (720x1280 portrait, RGB565)
// No font engine — uses a basic 5x7 pixel font rendered directly to framebuffer

#include "dsi_modal.h"
#include "dsi_display.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_cache.h"
#include <cstring>
#include <cstdio>

// Basic 5x7 font for ASCII 32-126 (uppercase, digits, symbols)
// Each char is 5 columns, each column is 7 bits (LSB=top)
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x00,0x00,0x5F,0x00,0x00}, // 33 !
    {0x00,0x07,0x00,0x07,0x00}, // 34 "
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 #
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 $
    {0x23,0x13,0x08,0x64,0x62}, // 37 %
    {0x36,0x49,0x55,0x22,0x50}, // 38 &
    {0x00,0x05,0x03,0x00,0x00}, // 39 '
    {0x00,0x1C,0x22,0x41,0x00}, // 40 (
    {0x00,0x41,0x22,0x1C,0x00}, // 41 )
    {0x14,0x08,0x3E,0x08,0x14}, // 42 *
    {0x08,0x08,0x3E,0x08,0x08}, // 43 +
    {0x00,0x50,0x30,0x00,0x00}, // 44 ,
    {0x08,0x08,0x08,0x08,0x08}, // 45 -
    {0x00,0x60,0x60,0x00,0x00}, // 46 .
    {0x20,0x10,0x08,0x04,0x02}, // 47 /
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 0
    {0x00,0x42,0x7F,0x40,0x00}, // 49 1
    {0x42,0x61,0x51,0x49,0x46}, // 50 2
    {0x21,0x41,0x45,0x4B,0x31}, // 51 3
    {0x18,0x14,0x12,0x7F,0x10}, // 52 4
    {0x27,0x45,0x45,0x45,0x39}, // 53 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 6
    {0x01,0x71,0x09,0x05,0x03}, // 55 7
    {0x36,0x49,0x49,0x49,0x36}, // 56 8
    {0x06,0x49,0x49,0x29,0x1E}, // 57 9
    {0x00,0x36,0x36,0x00,0x00}, // 58 :
    {0x00,0x56,0x36,0x00,0x00}, // 59 ;
    {0x08,0x14,0x22,0x41,0x00}, // 60 <
    {0x14,0x14,0x14,0x14,0x14}, // 61 =
    {0x00,0x41,0x22,0x14,0x08}, // 62 >
    {0x02,0x01,0x51,0x09,0x06}, // 63 ?
    {0x32,0x49,0x79,0x41,0x3E}, // 64 @
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 A
    {0x7F,0x49,0x49,0x49,0x36}, // 66 B
    {0x3E,0x41,0x41,0x41,0x22}, // 67 C
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 D
    {0x7F,0x49,0x49,0x49,0x41}, // 69 E
    {0x7F,0x09,0x09,0x09,0x01}, // 70 F
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 G
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 H
    {0x00,0x41,0x7F,0x41,0x00}, // 73 I
    {0x20,0x40,0x41,0x3F,0x01}, // 74 J
    {0x7F,0x08,0x14,0x22,0x41}, // 75 K
    {0x7F,0x40,0x40,0x40,0x40}, // 76 L
    {0x7F,0x02,0x0C,0x02,0x7F}, // 77 M
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 N
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 O
    {0x7F,0x09,0x09,0x09,0x06}, // 80 P
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 Q
    {0x7F,0x09,0x19,0x29,0x46}, // 82 R
    {0x46,0x49,0x49,0x49,0x31}, // 83 S
    {0x01,0x01,0x7F,0x01,0x01}, // 84 T
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 U
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 V
    {0x3F,0x40,0x38,0x40,0x3F}, // 87 W
    {0x63,0x14,0x08,0x14,0x63}, // 88 X
    {0x07,0x08,0x70,0x08,0x07}, // 89 Y
    {0x61,0x51,0x49,0x45,0x43}, // 90 Z
    {0x00,0x7F,0x41,0x41,0x00}, // 91 [
    {0x02,0x04,0x08,0x10,0x20}, // 92 backslash
    {0x00,0x41,0x41,0x7F,0x00}, // 93 ]
    {0x04,0x02,0x01,0x02,0x04}, // 94 ^
    {0x40,0x40,0x40,0x40,0x40}, // 95 _
    {0x00,0x01,0x02,0x04,0x00}, // 96 `
    {0x20,0x54,0x54,0x54,0x78}, // 97 a
    {0x7F,0x48,0x44,0x44,0x38}, // 98 b
    {0x38,0x44,0x44,0x44,0x20}, // 99 c
    {0x38,0x44,0x44,0x48,0x7F}, // 100 d
    {0x38,0x54,0x54,0x54,0x18}, // 101 e
    {0x08,0x7E,0x09,0x01,0x02}, // 102 f
    {0x0C,0x52,0x52,0x52,0x3E}, // 103 g
    {0x7F,0x08,0x04,0x04,0x78}, // 104 h
    {0x00,0x44,0x7D,0x40,0x00}, // 105 i
    {0x20,0x40,0x44,0x3D,0x00}, // 106 j
    {0x7F,0x10,0x28,0x44,0x00}, // 107 k
    {0x00,0x41,0x7F,0x40,0x00}, // 108 l
    {0x7C,0x04,0x18,0x04,0x78}, // 109 m
    {0x7C,0x08,0x04,0x04,0x78}, // 110 n
    {0x38,0x44,0x44,0x44,0x38}, // 111 o
    {0x7C,0x14,0x14,0x14,0x08}, // 112 p
    {0x08,0x14,0x14,0x18,0x7C}, // 113 q
    {0x7C,0x08,0x04,0x04,0x08}, // 114 r
    {0x48,0x54,0x54,0x54,0x20}, // 115 s
    {0x04,0x3F,0x44,0x40,0x20}, // 116 t
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 u
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 v
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 w
    {0x44,0x28,0x10,0x28,0x44}, // 120 x
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 y
    {0x44,0x64,0x54,0x4C,0x44}, // 122 z
    {0x00,0x08,0x36,0x41,0x00}, // 123 {
    {0x00,0x00,0x7F,0x00,0x00}, // 124 |
    {0x00,0x41,0x36,0x08,0x00}, // 125 }
    {0x10,0x08,0x08,0x10,0x08}, // 126 ~
};

// Draw a single character at (x,y) with scale factor, on RGB565 framebuffer
static void draw_char(uint16_t *fb, int fb_w, int fb_h,
                      int x, int y, char c, uint16_t color, int scale)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < fb_w && py >= 0 && py < fb_h) {
                            fb[py * fb_w + px] = color;
                        }
                    }
                }
            }
        }
    }
}

// Draw string at (x,y)
static void draw_string(uint16_t *fb, int fb_w, int fb_h,
                         int x, int y, const char *str, uint16_t color, int scale)
{
    int cx = x;
    for (const char *p = str; *p; p++) {
        draw_char(fb, fb_w, fb_h, cx, y, *p, color, scale);
        cx += 6 * scale;  // 5px char + 1px gap
    }
}

// Get string width in pixels
static int string_width(const char *str, int scale) {
    int len = strlen(str);
    return len > 0 ? len * 6 * scale - scale : 0;
}

// Fill rectangle
static void fill_rect(uint16_t *fb, int fb_w, int fb_h,
                       int x, int y, int w, int h, uint16_t color)
{
    for (int py = y; py < y + h && py < fb_h; py++) {
        if (py < 0) continue;
        for (int px = x; px < x + w && px < fb_w; px++) {
            if (px < 0) continue;
            fb[py * fb_w + px] = color;
        }
    }
}

// RGB888 to RGB565
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

void dsi_modal_show(esp_lcd_panel_handle_t panel, const char *title, const char *detail)
{
    void *fb0 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) != ESP_OK || !fb0) return;
    uint16_t *fb = (uint16_t *)fb0;

    const int W = DSI_LCD_W;  // 720
    const int H = DSI_LCD_H;  // 1280

    // Modal box dimensions
    int box_w = 500;
    int box_h = detail ? 200 : 140;
    int box_x = (W - box_w) / 2;
    int box_y = (H - box_h) / 2;

    // Dark background overlay (dim entire screen)
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = fb[y * W + x];
            // Darken: halve each channel
            fb[y * W + x] = ((px >> 1) & 0x7BEF);
        }
    }

    // Modal box: dark blue-gray background
    uint16_t box_bg = rgb565(30, 30, 50);
    uint16_t border = rgb565(80, 120, 200);
    fill_rect(fb, W, H, box_x - 2, box_y - 2, box_w + 4, box_h + 4, border);
    fill_rect(fb, W, H, box_x, box_y, box_w, box_h, box_bg);

    // Title text (scale 3 = 21px tall)
    if (title) {
        int tw = string_width(title, 3);
        int tx = box_x + (box_w - tw) / 2;
        int ty = box_y + 30;
        draw_string(fb, W, H, tx, ty, title, rgb565(255, 255, 255), 3);
    }

    // Detail text (scale 2 = 14px tall)
    if (detail) {
        int dw = string_width(detail, 2);
        int dx = box_x + (box_w - dw) / 2;
        int dy = box_y + 90;
        draw_string(fb, W, H, dx, dy, detail, rgb565(180, 180, 200), 2);
    }

    esp_cache_msync(fb0, W * H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void dsi_modal_progress(esp_lcd_panel_handle_t panel, const char *title,
                        int current, int total)
{
    void *fb0 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) != ESP_OK || !fb0) return;
    uint16_t *fb = (uint16_t *)fb0;

    const int W = DSI_LCD_W;
    const int H = DSI_LCD_H;

    int box_w = 500;
    int box_h = 200;
    int box_x = (W - box_w) / 2;
    int box_y = (H - box_h) / 2;

    uint16_t box_bg = rgb565(30, 30, 50);
    uint16_t border = rgb565(80, 120, 200);
    fill_rect(fb, W, H, box_x - 2, box_y - 2, box_w + 4, box_h + 4, border);
    fill_rect(fb, W, H, box_x, box_y, box_w, box_h, box_bg);

    // Title
    if (title) {
        int tw = string_width(title, 3);
        int tx = box_x + (box_w - tw) / 2;
        draw_string(fb, W, H, tx, box_y + 25, title, rgb565(255, 255, 255), 3);
    }

    // Progress bar
    int bar_x = box_x + 40;
    int bar_y = box_y + 90;
    int bar_w = box_w - 80;
    int bar_h = 30;

    // Background
    fill_rect(fb, W, H, bar_x, bar_y, bar_w, bar_h, rgb565(60, 60, 80));

    // Fill
    if (total > 0) {
        int fill_w = (int)((int64_t)bar_w * current / total);
        if (fill_w > bar_w) fill_w = bar_w;
        fill_rect(fb, W, H, bar_x, bar_y, fill_w, bar_h, rgb565(60, 180, 80));
    }

    // Percentage text
    char pct[32];
    if (total > 0) {
        snprintf(pct, sizeof(pct), "%d / %d  (%d%%)", current, total,
                 (int)((int64_t)100 * current / total));
    } else {
        snprintf(pct, sizeof(pct), "%d ...", current);
    }
    int pw = string_width(pct, 2);
    int px = box_x + (box_w - pw) / 2;
    draw_string(fb, W, H, px, bar_y + bar_h + 15, pct, rgb565(200, 200, 220), 2);

    esp_cache_msync(fb0, W * H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void dsi_banner(esp_lcd_panel_handle_t panel, const char *text)
{
    if (!text) return;
    void *fb0 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) != ESP_OK || !fb0) return;
    uint16_t *fb = (uint16_t *)fb0;

    const int W = DSI_LCD_W;
    const int H = DSI_LCD_H;
    const int bar_h = 50;
    const int bar_y = H - bar_h;

    // Semi-dark bar at bottom
    for (int y = bar_y; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = fb[y * W + x];
            fb[y * W + x] = ((px >> 1) & 0x7BEF);
        }
    }

    // Centered text (scale 3)
    int tw = string_width(text, 3);
    int tx = (W - tw) / 2;
    int ty = bar_y + (bar_h - 21) / 2;
    draw_string(fb, W, H, tx, ty, text, rgb565(255, 255, 255), 3);
}
