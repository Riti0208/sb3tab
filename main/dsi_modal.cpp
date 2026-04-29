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
static const uint16_t CLR_CHROME_BG  = rgb565(0x1F, 0x4F, 0x99);  // matches LVGL chrome bar
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

    // Use the same scale convention as lv_tiny_ttf (which LVGL menu chrome
    // uses) so a numeric "size" value renders identical glyph heights on
    // both paths. Earlier we used ScaleForPixelHeight which is ~1.39x smaller
    // for NotoSansJP — that's why the menu chrome looked bigger than the
    // DPI overlay at the same nominal size.
    float scale = stbtt_ScaleForMappingEmToPixels(&s_font, (float)size_px);
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

// Returns the DPI back buffer (where the application should write pixels).
// With num_fbs=2 in dsi_display, this alternates each call to dsi_present().
static uint16_t *get_fb(esp_lcd_panel_handle_t panel)
{
    if (!panel) return nullptr;
    return (uint16_t *)dsi_get_back_fb();
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
        TextEntry *te = render_text(title, 32);
        if (te) {
            int tx = card_x + (card_w - te->w) / 2;
            int ty = card_y + (hdr_h - te->h) / 2 + 4;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Detail line below header
    if (detail) {
        TextEntry *te = render_text(detail, 23);
        if (te) {
            int tx = card_x + (card_w - te->w) / 2;
            int ty = card_y + hdr_h + (card_h - hdr_h - te->h) / 2;
            blit_text_land(fb, te, tx, ty, CLR_TEXT);
        }
    }

    flush_fb(fb);
    dsi_present(panel);
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
        TextEntry *te = render_text(title, 29);
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
    TextEntry *te = render_text(pct, 19);
    if (te) {
        int tx = card_x + (card_w - te->w) / 2;
        int ty = bar_y + bar_h + 30;
        blit_text_land(fb, te, tx, ty, CLR_TEXT);
    }

    flush_fb(fb);
    dsi_present(panel);
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

// num_fbs=2 means each call alternates back buffers — there's no longer a
// stable "drawn once" state to short-circuit on. Kept as a no-op so callers
// (which were written for the num_fbs=1 era) compile unchanged.
void dsi_qr_overlay_invalidate() {}

void dsi_qr_overlay(esp_lcd_panel_handle_t panel,
                    const char *headline, const char *hint,
                    int bottom_strip_h)
{
    if (bottom_strip_h <= 0) return;

    // With num_fbs=2 the back buffer alternates each present, so the args
    // cache from the num_fbs=1 era no longer applies — every call has to
    // redraw on the new back buffer (the strip on the OTHER buffer is from
    // two frames ago). The *_invalidate API stays as a no-op for callers.
    uint16_t *fb = get_fb(panel);
    if (!fb) return;

    int strip_top_ly = LAND_H - bottom_strip_h;
    int draw_h = bottom_strip_h;

    for (int lx = 0; lx < LAND_W; lx++) {
        size_t base = (size_t)lx * DSI_LCD_W;
        for (int px = 0; px < draw_h; px++) {
            fb[base + px] = CLR_CHROME_BG;
        }
    }

    // 2-line layout: headline centered (line 1), hint right-aligned smaller
    // and a touch yellow (line 2). Tuned for ~88px strip; on smaller strips
    // we still center the headline and tuck the hint near the right edge.
    TextEntry *te_head = (headline && *headline) ? render_text(headline, 26) : nullptr;
    TextEntry *te_hint = (hint     && *hint)     ? render_text(hint,     16) : nullptr;

    int total_h = (te_head ? te_head->h : 0)
                + (te_hint ? te_hint->h + 4 : 0);
    int stack_top = strip_top_ly + (bottom_strip_h - total_h) / 2;
    if (stack_top < strip_top_ly) stack_top = strip_top_ly;

    if (te_head) {
        int tx = (LAND_W - te_head->w) / 2;
        int ty = stack_top;
        blit_text_land(fb, te_head, tx, ty, CLR_WHITE);
    }
    if (te_hint) {
        int tx = LAND_W - te_hint->w - 36;
        int ty = stack_top + (te_head ? te_head->h + 4 : 0);
        blit_text_land(fb, te_hint, tx, ty, CLR_YELLOW);
    }

    // Flush the touched portrait region.
    size_t row_bytes = (size_t)draw_h * 2;
    for (int py = 0; py < DSI_LCD_H; py++) {
        size_t off = (size_t)py * DSI_LCD_W * 2;
        esp_cache_msync((uint8_t *)fb + off, row_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}

// ============================================================
// Public: Switch-style top bar (clock + battery) for QR scan
// ============================================================

// See dsi_qr_overlay_invalidate notes — kept as no-op.
void dsi_qr_top_bar_invalidate() {}

void dsi_qr_top_bar(esp_lcd_panel_handle_t panel,
                    const char *clock_text,
                    const char *batt_text,
                    int batt_pct,
                    bool batt_charging,
                    int top_strip_h)
{
    if (top_strip_h <= 0) return;

    uint16_t *fb = get_fb(panel);
    if (!fb) return;

    // Top strip occupies landscape ly[0..top_strip_h-1], which after 90°-CW
    // rotation maps to portrait px[LAND_H-top_strip_h..LAND_H-1] for every
    // portrait row. Same chrome dark-blue (#1F4F99) as the LVGL menu bars
    // so the QR scan and menu screens read as one design.
    int draw_h = top_strip_h;
    for (int lx = 0; lx < LAND_W; lx++) {
        size_t base = (size_t)lx * DSI_LCD_W;
        for (int ly = 0; ly < draw_h; ly++) {
            int px = LAND_H - 1 - ly;
            fb[base + px] = CLR_CHROME_BG;
        }
    }

    // Clock — left-aligned, white. Match the LVGL chrome (Montserrat 24)
    // so the QR view and the menu chrome read the same.
    if (clock_text && *clock_text) {
        TextEntry *te = render_text(clock_text, 24);
        if (te) {
            int tx = 24;
            int ty = (top_strip_h - te->h) / 2;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Battery — right-aligned. Layout: [icon ▭][gap][text "100%"]. Icon is a
    // 30x14 rounded outlined rect with a 4px nub on the right and a fill bar
    // proportional to batt_pct.
    if (batt_pct >= 0) {
        TextEntry *te = (batt_text && *batt_text) ? render_text(batt_text, 24) : nullptr;
        const int ICON_W = 30, ICON_H = 14, NUB_W = 4, NUB_H = 6;
        const int GAP = 6;
        int total_w = ICON_W + NUB_W + GAP + (te ? te->w : 0);
        int x = LAND_W - total_w - 24;
        int icon_y = (top_strip_h - ICON_H) / 2;

        // Outline (1 px white)
        fill_rect_land(fb, x, icon_y, ICON_W, 1, CLR_WHITE);                // top
        fill_rect_land(fb, x, icon_y + ICON_H - 1, ICON_W, 1, CLR_WHITE);    // bottom
        fill_rect_land(fb, x, icon_y, 1, ICON_H, CLR_WHITE);                 // left
        fill_rect_land(fb, x + ICON_W - 1, icon_y, 1, ICON_H, CLR_WHITE);    // right
        // Positive terminal nub
        fill_rect_land(fb, x + ICON_W, icon_y + (ICON_H - NUB_H) / 2,
                        NUB_W, NUB_H, CLR_WHITE);
        // Fill bar (inset 2 px). Charging shows full green; otherwise white.
        int fill_w_max = ICON_W - 4;
        int fill_w = (batt_pct >= 0 && batt_pct <= 100)
                     ? (fill_w_max * batt_pct / 100) : 0;
        if (fill_w > 0) {
            uint16_t fill_col = batt_charging ? CLR_GREEN : CLR_WHITE;
            fill_rect_land(fb, x + 2, icon_y + 2, fill_w, ICON_H - 4, fill_col);
        }

        if (te) {
            int tx = x + ICON_W + NUB_W + GAP;
            int ty = (top_strip_h - te->h) / 2;
            blit_text_land(fb, te, tx, ty, CLR_WHITE);
        }
    }

    // Flush only the touched portrait region: rows [LAND_H-draw_h .. LAND_H-1]
    // sit at the high end of every portrait scanline.
    size_t row_bytes = (size_t)draw_h * 2;
    size_t row_off   = (size_t)(LAND_H - draw_h) * 2;
    for (int py = 0; py < DSI_LCD_H; py++) {
        size_t off = (size_t)py * DSI_LCD_W * 2 + row_off;
        esp_cache_msync((uint8_t *)fb + off, row_bytes,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                        ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
}
