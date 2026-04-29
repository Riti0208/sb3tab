// Kid-friendly modal/QR overlays for Tab5 DSI (720x1280 portrait, RGB565).
// Drawing is done in a virtual landscape (1280x720) and rotated 90° CW into
// the portrait framebuffer. Uses stb_truetype to rasterize NotoSansJP into
// alpha bitmaps that are cached by (text, size_px) and alpha-blitted each
// call — so the per-frame QR overlay only pays the blend cost, not the
// rasterization cost.

#include "dsi_modal.h"
#include "dsi_display.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>

// stb_truetype (single-file lib) — STBTT_STATIC keeps symbols file-local so
// they don't collide with the copy already inside scratch_core. The header
// emits a bunch of static helpers we don't call; silence those warnings
// only for this translation unit.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x,u)  heap_caps_malloc((x), MALLOC_CAP_SPIRAM)
#define STBTT_free(x,u)    heap_caps_free((x))
#include "stb_truetype.h"
#pragma GCC diagnostic pop

// Embedded NotoSansJP TTF (linked via EMBED_FILES in scratch_core CMakeLists)
extern "C" const uint8_t noto_sans_ttf_start[] asm("_binary_NotoSansJP_Medium_subset_ttf_start");
extern "C" const uint8_t noto_sans_ttf_end[]   asm("_binary_NotoSansJP_Medium_subset_ttf_end");

// ============================================================
// Landscape <-> portrait mapping
// ============================================================

#define LAND_W  DSI_LCD_H   // 1280 — visible width when held landscape
#define LAND_H  DSI_LCD_W   // 720  — visible height when held landscape

static inline int port_index(int lx, int ly) {
    // Rotation 90° CW: landscape(lx,ly) -> portrait(LAND_H-1-ly, lx)
    return lx * DSI_LCD_W + (LAND_H - 1 - ly);
}

static inline void put_px_land(uint16_t *fb, int lx, int ly, uint16_t color) {
    if ((unsigned)lx >= (unsigned)LAND_W || (unsigned)ly >= (unsigned)LAND_H) return;
    fb[port_index(lx, ly)] = color;
}

static inline uint16_t get_px_land(uint16_t *fb, int lx, int ly) {
    if ((unsigned)lx >= (unsigned)LAND_W || (unsigned)ly >= (unsigned)LAND_H) return 0;
    return fb[port_index(lx, ly)];
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// ============================================================
// Scratch-themed colors
// ============================================================

static const uint16_t CLR_BLUE       = rgb565(0x4C, 0x97, 0xFF);  // SCRATCH_BLUE
static const uint16_t CLR_BLUE_DARK  = rgb565(0x33, 0x73, 0xCC);
static const uint16_t CLR_YELLOW     = rgb565(0xFF, 0xD8, 0x3D);  // reticle
static const uint16_t CLR_ORANGE     = rgb565(0xFF, 0x8C, 0x1A);
static const uint16_t CLR_WHITE      = rgb565(0xFF, 0xFF, 0xFF);
static const uint16_t CLR_TEXT       = rgb565(0x57, 0x5E, 0x75);  // SCRATCH_TEXT
static const uint16_t CLR_GREEN      = rgb565(0x4C, 0xBF, 0x56);

// ============================================================
// Geometry helpers (landscape coords)
// ============================================================

static void fill_rect_land(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > LAND_W) x1 = LAND_W;
    int y1 = y + h; if (y1 > LAND_H) y1 = LAND_H;
    for (int ly = y0; ly < y1; ly++) {
        for (int lx = x0; lx < x1; lx++) {
            fb[port_index(lx, ly)] = color;
        }
    }
}

static void fill_rounded_rect(uint16_t *fb, int x, int y, int w, int h,
                               int radius, uint16_t color)
{
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius == 0) { fill_rect_land(fb, x, y, w, h, color); return; }

    // Center + edges
    fill_rect_land(fb, x + radius, y, w - 2 * radius, h, color);
    fill_rect_land(fb, x, y + radius, radius, h - 2 * radius, color);
    fill_rect_land(fb, x + w - radius, y + radius, radius, h - 2 * radius, color);

    // Quarter-circle corners
    int r2 = radius * radius;
    for (int cy = 0; cy < radius; cy++) {
        int dy = radius - cy;
        for (int cx = 0; cx < radius; cx++) {
            int dx = radius - cx;
            if (dx * dx + dy * dy <= r2) {
                put_px_land(fb, x + cx,             y + cy,             color);
                put_px_land(fb, x + w - 1 - cx,     y + cy,             color);
                put_px_land(fb, x + cx,             y + h - 1 - cy,     color);
                put_px_land(fb, x + w - 1 - cx,     y + h - 1 - cy,     color);
            }
        }
    }
}

// Dim every pixel by halving each channel — cheap "modal backdrop" effect.
static void dim_full_screen(uint16_t *fb)
{
    // Walk the framebuffer linearly; we don't care about coordinates here.
    uint16_t *p = fb;
    int n = DSI_LCD_W * DSI_LCD_H;
    for (int i = 0; i < n; i++) {
        p[i] = (p[i] >> 1) & 0x7BEF;
    }
}

// ============================================================
// stb_truetype font + per-string alpha bitmap cache
// ============================================================

static stbtt_fontinfo s_font;
static bool s_font_ready = false;

static bool font_init()
{
    if (s_font_ready) return true;
    int off = stbtt_GetFontOffsetForIndex(noto_sans_ttf_start, 0);
    if (off < 0) return false;
    if (!stbtt_InitFont(&s_font, noto_sans_ttf_start, off)) return false;
    s_font_ready = true;
    return true;
}

// Decode one UTF-8 codepoint and advance the cursor.
static uint32_t utf8_next(const char *&p)
{
    uint8_t c = (uint8_t)*p;
    uint32_t cp;
    int extra;
    if (c < 0x80)      { cp = c;        extra = 0; }
    else if (c < 0xC0) { cp = 0xFFFD;   extra = 0; }
    else if (c < 0xE0) { cp = c & 0x1F; extra = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; extra = 2; }
    else               { cp = c & 0x07; extra = 3; }
    if (*p) p++;
    for (int j = 0; j < extra && *p; j++, p++) {
        cp = (cp << 6) | ((uint8_t)*p & 0x3F);
    }
    return cp;
}

#define TEXT_CACHE_N      8
#define TEXT_CACHE_KEYLEN 96

struct TextEntry {
    char     text[TEXT_CACHE_KEYLEN];
    int      size_px;
    int      w;
    int      h;
    uint8_t *alpha;       // PSRAM, w*h bytes
    uint64_t last_used;
};

static TextEntry s_text_cache[TEXT_CACHE_N] = {};
static uint64_t  s_tick = 0;

static TextEntry *render_text(const char *text, int size_px)
{
    if (!text || !*text) return nullptr;
    if (!font_init()) return nullptr;
    s_tick++;

    // Lookup
    for (auto &e : s_text_cache) {
        if (e.alpha && e.size_px == size_px &&
            strncmp(e.text, text, sizeof(e.text)) == 0) {
            e.last_used = s_tick;
            return &e;
        }
    }

    float scale = stbtt_ScaleForPixelHeight(&s_font, (float)size_px);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &lineGap);
    int baseline = (int)(scale * ascent + 0.5f);
    int height = (int)(scale * (ascent - descent) + 0.5f) + 4;

    // Measure
    float adv_total = 0;
    {
        const char *p = text;
        uint32_t prev = 0;
        while (*p) {
            uint32_t cp = utf8_next(p);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
            if (prev) adv_total += scale * stbtt_GetCodepointKernAdvance(&s_font, prev, cp);
            adv_total += scale * adv;
            prev = cp;
        }
    }
    int width = (int)(adv_total + 0.5f) + 2;
    if (width <= 0 || width > 1200 || height <= 0) return nullptr;

    uint8_t *alpha = (uint8_t *)heap_caps_calloc(width * height, 1, MALLOC_CAP_SPIRAM);
    if (!alpha) return nullptr;

    // Rasterize
    {
        const char *p = text;
        uint32_t prev = 0;
        float curX = 0;
        while (*p) {
            uint32_t cp = utf8_next(p);
            if (prev) curX += scale * stbtt_GetCodepointKernAdvance(&s_font, prev, cp);

            int x0, y0, x1, y1;
            stbtt_GetCodepointBitmapBox(&s_font, cp, scale, scale,
                                        &x0, &y0, &x1, &y1);
            int gw = x1 - x0;
            int gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw < 256 && gh < 256) {
                uint8_t *gb = (uint8_t *)malloc(gw * gh);
                if (gb) {
                    stbtt_MakeCodepointBitmap(&s_font, gb, gw, gh, gw,
                                              scale, scale, cp);
                    int dx0 = (int)(curX + 0.5f) + x0;
                    int dy0 = baseline + y0;
                    for (int gy = 0; gy < gh; gy++) {
                        int dy = dy0 + gy;
                        if ((unsigned)dy >= (unsigned)height) continue;
                        for (int gx = 0; gx < gw; gx++) {
                            int dx = dx0 + gx;
                            if ((unsigned)dx >= (unsigned)width) continue;
                            uint8_t a = gb[gy * gw + gx];
                            uint8_t *cell = &alpha[dy * width + dx];
                            if (a > *cell) *cell = a;
                        }
                    }
                    free(gb);
                }
            }
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
            curX += scale * adv;
            prev = cp;
        }
    }

    // Pick LRU slot
    TextEntry *slot = &s_text_cache[0];
    for (auto &e : s_text_cache) {
        if (!e.alpha) { slot = &e; break; }
        if (e.last_used < slot->last_used) slot = &e;
    }
    if (slot->alpha) heap_caps_free(slot->alpha);
    strncpy(slot->text, text, sizeof(slot->text) - 1);
    slot->text[sizeof(slot->text) - 1] = '\0';
    slot->size_px   = size_px;
    slot->w         = width;
    slot->h         = height;
    slot->alpha     = alpha;
    slot->last_used = s_tick;
    return slot;
}

// Alpha-blit a cached text bitmap onto the framebuffer at landscape (x,y)
// with the given foreground color. a≥250 paths short-circuit to opaque write.
static void blit_text_land(uint16_t *fb, const TextEntry *te,
                           int x, int y, uint16_t fg)
{
    if (!te || !te->alpha) return;

    uint8_t fr = ((fg >> 11) & 0x1F) * 255 / 31;
    uint8_t fG = ((fg >>  5) & 0x3F) * 255 / 63;
    uint8_t fB =  (fg        & 0x1F) * 255 / 31;

    for (int gy = 0; gy < te->h; gy++) {
        int ly = y + gy;
        if ((unsigned)ly >= (unsigned)LAND_H) continue;
        const uint8_t *row = &te->alpha[gy * te->w];
        for (int gx = 0; gx < te->w; gx++) {
            uint8_t a = row[gx];
            if (!a) continue;
            int lx = x + gx;
            if ((unsigned)lx >= (unsigned)LAND_W) continue;
            int idx = port_index(lx, ly);
            if (a >= 250) {
                fb[idx] = fg;
            } else {
                uint16_t bg = fb[idx];
                uint8_t br = ((bg >> 11) & 0x1F) * 255 / 31;
                uint8_t bG = ((bg >>  5) & 0x3F) * 255 / 63;
                uint8_t bB =  (bg        & 0x1F) * 255 / 31;
                uint8_t r = (fr * a + br * (255 - a)) >> 8;
                uint8_t g = (fG * a + bG * (255 - a)) >> 8;
                uint8_t b = (fB * a + bB * (255 - a)) >> 8;
                fb[idx] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            }
        }
    }
}

// Get framebuffer (DPI panel back buffer 1) with cache flush at end.
static uint16_t *get_fb(esp_lcd_panel_handle_t panel)
{
    void *fb0 = nullptr;
    if (!panel) return nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) != ESP_OK) return nullptr;
    return (uint16_t *)fb0;
}

static void flush_fb(uint16_t *fb)
{
    esp_cache_msync(fb, DSI_LCD_W * DSI_LCD_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// ============================================================
// Public: full-screen status modal (white card on dimmed bg)
// ============================================================

void dsi_modal_show(esp_lcd_panel_handle_t panel, const char *title, const char *detail)
{
    uint16_t *fb = get_fb(panel);
    if (!fb) return;

    dim_full_screen(fb);

    // Card
    int card_w = 720;
    int card_h = detail ? 280 : 200;
    int card_x = (LAND_W - card_w) / 2;
    int card_y = (LAND_H - card_h) / 2;

    // Soft drop shadow (offset, semi-dark)
    fill_rounded_rect(fb, card_x + 6, card_y + 8, card_w, card_h, 36,
                      rgb565(20, 30, 60));
    // White card
    fill_rounded_rect(fb, card_x, card_y, card_w, card_h, 36, CLR_WHITE);

    // Blue header strip (top portion of the card)
    int hdr_h = 110;
    fill_rounded_rect(fb, card_x, card_y, card_w, hdr_h, 36, CLR_BLUE);
    // Square off the bottom of the header so it meets the card body cleanly
    fill_rect_land(fb, card_x + 4, card_y + hdr_h - 36, card_w - 8, 36, CLR_BLUE);

    // Title in the blue header
    if (title) {
        TextEntry *te = render_text(title, 44);
        if (te) {
            int tx = card_x + (card_w - te->w) / 2;
            int ty = card_y + (hdr_h - te->h) / 2 + 4;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Detail line below header
    if (detail) {
        TextEntry *te = render_text(detail, 32);
        if (te) {
            int tx = card_x + (card_w - te->w) / 2;
            int ty = card_y + hdr_h + (card_h - hdr_h - te->h) / 2;
            blit_text_land(fb, te, tx, ty, CLR_TEXT);
        }
    }

    flush_fb(fb);
}

// ============================================================
// Public: progress modal with bar
// ============================================================

void dsi_modal_progress(esp_lcd_panel_handle_t panel, const char *title,
                        int current, int total)
{
    uint16_t *fb = get_fb(panel);
    if (!fb) return;

    dim_full_screen(fb);

    int card_w = 760;
    int card_h = 320;
    int card_x = (LAND_W - card_w) / 2;
    int card_y = (LAND_H - card_h) / 2;

    fill_rounded_rect(fb, card_x + 6, card_y + 8, card_w, card_h, 36,
                      rgb565(20, 30, 60));
    fill_rounded_rect(fb, card_x, card_y, card_w, card_h, 36, CLR_WHITE);

    int hdr_h = 110;
    fill_rounded_rect(fb, card_x, card_y, card_w, hdr_h, 36, CLR_BLUE);
    fill_rect_land(fb, card_x + 4, card_y + hdr_h - 36, card_w - 8, 36, CLR_BLUE);

    if (title) {
        TextEntry *te = render_text(title, 40);
        if (te) {
            int tx = card_x + (card_w - te->w) / 2;
            int ty = card_y + (hdr_h - te->h) / 2 + 4;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Progress bar: rounded track + green fill
    int bar_w = card_w - 80;
    int bar_h = 36;
    int bar_x = card_x + 40;
    int bar_y = card_y + hdr_h + 50;
    fill_rounded_rect(fb, bar_x, bar_y, bar_w, bar_h, bar_h / 2, rgb565(228, 232, 240));
    if (total > 0 && current > 0) {
        int fill_w = (int)((int64_t)bar_w * current / total);
        if (fill_w > bar_w) fill_w = bar_w;
        if (fill_w >= bar_h)  // only round the fill once it's wider than the cap
            fill_rounded_rect(fb, bar_x, bar_y, fill_w, bar_h, bar_h / 2, CLR_GREEN);
        else if (fill_w > 0)
            fill_rect_land(fb, bar_x + bar_h / 2, bar_y, fill_w, bar_h, CLR_GREEN);
    }

    char pct[64];
    if (total > 0) {
        int percent = (int)((int64_t)100 * current / total);
        snprintf(pct, sizeof(pct), "%d / %d  (%d%%)", current, total, percent);
    } else {
        snprintf(pct, sizeof(pct), "%d ...", current);
    }
    TextEntry *te = render_text(pct, 26);
    if (te) {
        int tx = card_x + (card_w - te->w) / 2;
        int ty = bar_y + bar_h + 30;
        blit_text_land(fb, te, tx, ty, CLR_TEXT);
    }

    flush_fb(fb);
}

// ============================================================
// Public: kid-friendly QR scan overlay
//
// num_fbs=1 means every CPU write to the framebuffer is racing the DPI
// scan-out. Trying to redraw the overlay every camera frame produces
// horrible flicker, because PPA SRM (camera) overwrites the entire FB
// each frame and we'd have to re-paint our overlay back on top in a
// race against the DPI.
//
// Instead we make this a *one-shot* overlay: we render a solid blue
// strip at the bottom of the landscape view (= portrait LEFT edge) once
// per scan session and msync it. The camera preview is carved to skip
// that strip (camera_set_preview_strips() configures PPA SRM's output
// rectangle), so subsequent camera frames never touch the strip and the
// overlay stays stable with zero per-frame work.
// ============================================================

// Track the strip we last drew so a repeat call with the same args becomes
// a no-op. Caller can flag via dsi_qr_overlay_invalidate() when something
// else (e.g. a status modal) has overwritten the strip in the meantime.
static char s_strip_headline[TEXT_CACHE_KEYLEN] = {};
static char s_strip_hint    [TEXT_CACHE_KEYLEN] = {};
static int  s_strip_h       = -1;
static bool s_strip_drawn   = false;

void dsi_qr_overlay_invalidate()
{
    s_strip_drawn = false;
}

void dsi_qr_overlay(esp_lcd_panel_handle_t panel,
                    const char *headline, const char *hint,
                    int bottom_strip_h)
{
    if (bottom_strip_h <= 0) return;

    const char *h_now = headline ? headline : "";
    const char *t_now = hint     ? hint     : "";
    if (s_strip_drawn &&
        s_strip_h == bottom_strip_h &&
        strncmp(s_strip_headline, h_now, sizeof(s_strip_headline)) == 0 &&
        strncmp(s_strip_hint,     t_now, sizeof(s_strip_hint))     == 0) {
        return;  // Already on screen.
    }

    uint16_t *fb = get_fb(panel);
    if (!fb) return;

    // Strip occupies landscape ly[LAND_H - bottom_strip_h .. LAND_H - 1],
    // which after the 90°-CW rotation is portrait px[0 .. bottom_strip_h - 1]
    // for every portrait row. We iterate landscape-X outer (= portrait-Y
    // outer) so each iteration writes a contiguous run of bytes inside one
    // portrait row, which is cache-friendly.
    int strip_top_ly = LAND_H - bottom_strip_h;
    uint16_t accent  = rgb565(0x33, 0x73, 0xCC);  // CLR_BLUE_DARK, top divider

    for (int lx = 0; lx < LAND_W; lx++) {
        // Convert landscape column lx → portrait row py=lx, px range
        // [0, bottom_strip_h - 1] (=719-ly walks from 0 upward as ly walks
        // down from LAND_H-1).
        size_t base = (size_t)lx * DSI_LCD_W;
        for (int px = 0; px < bottom_strip_h; px++) {
            // px=0 corresponds to landscape ly=LAND_H-1 (very bottom);
            // px=bottom_strip_h-1 is one row below the divider.
            fb[base + px] = CLR_BLUE;
        }
        // 1-pixel darker accent line at the very top of the strip
        fb[base + bottom_strip_h - 1] = accent;
    }

    // Headline centered horizontally in the strip
    if (headline && *headline) {
        TextEntry *te = render_text(headline, 38);
        if (te) {
            int tx = (LAND_W - te->w) / 2;
            int ty = strip_top_ly + (bottom_strip_h - te->h) / 2;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Hint right-aligned, smaller
    if (hint && *hint) {
        TextEntry *te = render_text(hint, 24);
        if (te) {
            int tx = LAND_W - te->w - 36;
            int ty = strip_top_ly + (bottom_strip_h - te->h) / 2 + 1;
            blit_text_land(fb, te, tx, ty, CLR_YELLOW);
        }
    }

    // Flush the touched portrait region. The strip lives in portrait
    // px[0..bottom_strip_h-1] across every row, so cover full portrait
    // height with a tight column-stride msync per row. With ~110 px
    // strip × 1280 rows, that's ~280 KB total — much smaller than the
    // 1.8 MB full-FB flush, and it's a one-shot cost (we won't redraw
    // until the strip is invalidated).
    size_t row_bytes = (size_t)bottom_strip_h * 2;
    for (int py = 0; py < DSI_LCD_H; py++) {
        size_t off = (size_t)py * DSI_LCD_W * 2;
        esp_cache_msync((uint8_t *)fb + off, row_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    strncpy(s_strip_headline, h_now, sizeof(s_strip_headline) - 1);
    s_strip_headline[sizeof(s_strip_headline) - 1] = '\0';
    strncpy(s_strip_hint, t_now, sizeof(s_strip_hint) - 1);
    s_strip_hint[sizeof(s_strip_hint) - 1] = '\0';
    s_strip_h     = bottom_strip_h;
    s_strip_drawn = true;
}
