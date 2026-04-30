#include "speech_manager_headless.hpp"
#include <runtime.hpp>
#include <render.hpp>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef __ESP32__
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#endif

// ============================================================
// stb_truetype font renderer
// ============================================================

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
// Use PSRAM for temp allocations in stb_truetype
#ifdef __ESP32__
#include "esp_heap_caps.h"
#define STBTT_malloc(x,u)  heap_caps_malloc(x, MALLOC_CAP_SPIRAM)
#define STBTT_free(x,u)    heap_caps_free(x)
#endif
#include "stb_truetype.h"

// Embedded font (linked via EMBED_FILES in CMakeLists.txt)
extern const uint8_t noto_sans_ttf_start[] asm("_binary_NotoSansJP_Medium_subset_ttf_start");
extern const uint8_t noto_sans_ttf_end[]   asm("_binary_NotoSansJP_Medium_subset_ttf_end");

static stbtt_fontinfo g_font;
bool g_font_loaded = false;
float g_font_scale = 0;
static const float FONT_SIZE = 14.0f; // pixels

void ensureFontLoaded() {
    if (g_font_loaded) return;

#ifdef __ESP32__
    // Verify font data is accessible
    if (noto_sans_ttf_start == nullptr || noto_sans_ttf_end <= noto_sans_ttf_start) {
        return;
    }
#endif

    int offset = stbtt_GetFontOffsetForIndex(noto_sans_ttf_start, 0);
    if (offset < 0) {
        return;
    }

    if (!stbtt_InitFont(&g_font, noto_sans_ttf_start, offset)) {
        return;
    }
    g_font_scale = stbtt_ScaleForPixelHeight(&g_font, FONT_SIZE);
    g_font_loaded = true;
}

// Decode one UTF-8 codepoint, advance index
static uint32_t decodeUtf8(const std::string &s, size_t &i) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80)       { cp = c;           extra = 0; }
    else if (c < 0xC0)  { cp = 0xFFFD;      extra = 0; } // invalid
    else if (c < 0xE0)  { cp = c & 0x1F;    extra = 1; }
    else if (c < 0xF0)  { cp = c & 0x0F;    extra = 2; }
    else                 { cp = c & 0x07;    extra = 3; }
    i++;
    for (int j = 0; j < extra && i < s.size(); j++, i++) {
        cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
    }
    return cp;
}

// Measure text width in pixels using stb_truetype
float measureTextWidthTTF(const std::string &text) {
    if (!g_font_loaded) return text.size() * 8.0f;
    float width = 0;
    size_t i = 0;
    uint32_t prevCp = 0;
    while (i < text.size()) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') break;

        int advW, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &advW, &lsb);
        if (prevCp) {
            width += g_font_scale * stbtt_GetCodepointKernAdvance(&g_font, prevCp, cp);
        }
        width += g_font_scale * advW;
        prevCp = cp;
    }
    return width;
}

// Render a string onto an RGB888 framebuffer with alpha blending
void drawStringTTF(uint8_t *fb, int fbW, int fbH,
                   int px, int py, const std::string &text,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (!g_font_loaded) return;

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &lineGap);
    float scaledAscent = g_font_scale * ascent;

    float curX = (float)px;
    int curY = py;
    size_t i = 0;
    uint32_t prevCp = 0;

    while (i < text.size()) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') {
            curX = (float)px;
            curY += (int)(g_font_scale * (ascent - descent + lineGap));
            prevCp = 0;
            continue;
        }

        if (prevCp) {
            curX += g_font_scale * stbtt_GetCodepointKernAdvance(&g_font, prevCp, cp);
        }

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, cp, g_font_scale, g_font_scale,
                                    &x0, &y0, &x1, &y1);

        int glyphW = x1 - x0;
        int glyphH = y1 - y0;

        if (glyphW > 0 && glyphH > 0 && glyphW < 256 && glyphH < 256) {
            uint8_t *glyphPtr = (uint8_t *)malloc(glyphW * glyphH);
            if (!glyphPtr) { prevCp = cp; continue; }

            stbtt_MakeCodepointBitmap(&g_font, glyphPtr, glyphW, glyphH, glyphW,
                                      g_font_scale, g_font_scale, cp);

            // Blit glyph with alpha blending
            int drawX = (int)(curX + 0.5f) + x0;
            int drawY = curY + (int)(scaledAscent + 0.5f) + y0;

            for (int gy = 0; gy < glyphH; gy++) {
                int dy = drawY + gy;
                if (dy < 0 || dy >= fbH) continue;
                for (int gx = 0; gx < glyphW; gx++) {
                    int dx = drawX + gx;
                    if (dx < 0 || dx >= fbW) continue;

                    uint8_t alpha = glyphPtr[gy * glyphW + gx];
                    if (alpha == 0) continue;

                    uint8_t *dst = fb + (dy * fbW + dx) * 3;
                    if (alpha >= 250) {
                        dst[0] = r; dst[1] = g; dst[2] = b;
                    } else {
                        dst[0] = (r * alpha + dst[0] * (255 - alpha)) >> 8;
                        dst[1] = (g * alpha + dst[1] * (255 - alpha)) >> 8;
                        dst[2] = (b * alpha + dst[2] * (255 - alpha)) >> 8;
                    }
                }
            }
            free(glyphPtr);
        }

        int advW, lsb;
        stbtt_GetCodepointHMetrics(&g_font, cp, &advW, &lsb);
        curX += g_font_scale * advW;
        prevCp = cp;
    }
}

// ============================================================
// Drawing helpers
// ============================================================

void fillRectFb(uint8_t *fb, int fbW, int fbH,
                int x, int y, int w, int h,
                uint8_t r, uint8_t g, uint8_t b) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(fbW, x + w), y1 = std::min(fbH, y + h);
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            uint8_t *dst = fb + (py * fbW + px) * 3;
            dst[0] = r; dst[1] = g; dst[2] = b;
        }
    }
}

static void fillRoundedRect(uint8_t *fb, int fbW, int fbH,
                             int x, int y, int w, int h, int radius,
                             uint8_t r, uint8_t g, uint8_t b) {
    radius = std::min(radius, std::min(w / 2, h / 2));
    // Center rect
    fillRectFb(fb, fbW, fbH, x + radius, y, w - 2 * radius, h, r, g, b);
    // Left/right columns
    fillRectFb(fb, fbW, fbH, x, y + radius, radius, h - 2 * radius, r, g, b);
    fillRectFb(fb, fbW, fbH, x + w - radius, y + radius, radius, h - 2 * radius, r, g, b);
    // Corners (quarter circles)
    for (int cy = 0; cy < radius; cy++) {
        for (int cx = 0; cx < radius; cx++) {
            float dist = sqrtf((float)((radius - cx) * (radius - cx) + (radius - cy) * (radius - cy)));
            if (dist <= radius + 0.5f) {
                // Top-left
                int px1 = x + cx, py1 = y + cy;
                if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
                    uint8_t *dst = fb + (py1 * fbW + px1) * 3;
                    dst[0] = r; dst[1] = g; dst[2] = b;
                }
                // Top-right
                px1 = x + w - 1 - cx;
                if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
                    uint8_t *dst = fb + (py1 * fbW + px1) * 3;
                    dst[0] = r; dst[1] = g; dst[2] = b;
                }
                // Bottom-left
                py1 = y + h - 1 - cy;
                px1 = x + cx;
                if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
                    uint8_t *dst = fb + (py1 * fbW + px1) * 3;
                    dst[0] = r; dst[1] = g; dst[2] = b;
                }
                // Bottom-right
                px1 = x + w - 1 - cx;
                if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
                    uint8_t *dst = fb + (py1 * fbW + px1) * 3;
                    dst[0] = r; dst[1] = g; dst[2] = b;
                }
            }
        }
    }
}

static void drawRoundedRectOutline(uint8_t *fb, int fbW, int fbH,
                                    int x, int y, int w, int h, int radius,
                                    uint8_t r, uint8_t g, uint8_t b) {
    radius = std::min(radius, std::min(w / 2, h / 2));
    // Top & bottom edges (excluding corners)
    fillRectFb(fb, fbW, fbH, x + radius, y, w - 2 * radius, 1, r, g, b);
    fillRectFb(fb, fbW, fbH, x + radius, y + h - 1, w - 2 * radius, 1, r, g, b);
    // Left & right edges
    fillRectFb(fb, fbW, fbH, x, y + radius, 1, h - 2 * radius, r, g, b);
    fillRectFb(fb, fbW, fbH, x + w - 1, y + radius, 1, h - 2 * radius, r, g, b);
    // Corner arcs
    for (int a = 0; a <= 90; a++) {
        float rad = a * M_PI / 180.0f;
        int cx = (int)(radius * cosf(rad) + 0.5f);
        int cy = (int)(radius * sinf(rad) + 0.5f);
        // TL
        int px1 = x + radius - cx, py1 = y + radius - cy;
        if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
            uint8_t *dst = fb + (py1 * fbW + px1) * 3; dst[0]=r; dst[1]=g; dst[2]=b;
        }
        // TR
        px1 = x + w - 1 - radius + cx;
        if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
            uint8_t *dst = fb + (py1 * fbW + px1) * 3; dst[0]=r; dst[1]=g; dst[2]=b;
        }
        // BL
        py1 = y + h - 1 - radius + cy; px1 = x + radius - cx;
        if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
            uint8_t *dst = fb + (py1 * fbW + px1) * 3; dst[0]=r; dst[1]=g; dst[2]=b;
        }
        // BR
        px1 = x + w - 1 - radius + cx;
        if (px1 >= 0 && px1 < fbW && py1 >= 0 && py1 < fbH) {
            uint8_t *dst = fb + (py1 * fbW + px1) * 3; dst[0]=r; dst[1]=g; dst[2]=b;
        }
    }
}

// ============================================================
// SpeechManagerHeadless
// ============================================================

SpeechManagerHeadless::SpeechManagerHeadless() {
    // Font loading deferred to first render to avoid crash during init
}

SpeechManagerHeadless::~SpeechManagerHeadless() {
    cleanup();
}

double SpeechManagerHeadless::getCurrentTime() {
#ifdef __ESP32__
    return (double)esp_timer_get_time() / 1000000.0;
#else
    return 0.0;
#endif
}

void SpeechManagerHeadless::createSpeechObject(Sprite *sprite, const std::string &message) {
    speechObjects[sprite] = std::make_unique<SpeechTextObjectHeadless>(message, 170);
}

void SpeechManagerHeadless::render(int offsetX, int offsetY) {
    // No-op: actual rendering done via renderToFramebuffer()
}

void SpeechManagerHeadless::renderToFramebuffer(uint8_t *fb, int fbW, int fbH,
                                                 int projectW, int projectH) {
    if (!fb) return;

    for (auto &[sprite, obj] : speechObjects) {
        if (!obj || !sprite->visible) continue;

        const std::string &text = obj->getText();
        if (text.empty()) continue;

        // Scratch coordinate -> framebuffer coordinate
        int spriteFbX = (int)(fbW / 2.0 + sprite->xPosition * fbW / (double)projectW);
        int spriteFbY = (int)(fbH / 2.0 - sprite->yPosition * fbH / (double)projectH);

        // Estimate sprite dimensions on screen
        int sprW = (int)(sprite->spriteWidth * sprite->size / 100.0 * fbW / (double)projectW);
        int sprH = (int)(sprite->spriteHeight * sprite->size / 100.0 * fbH / (double)projectH);

        // Measure text size using stb_truetype
        float maxLineW = 0;
        int lineCount = 1;
        {
            size_t lineStart = 0;
            for (size_t pos = 0; pos < text.size(); pos++) {
                if (text[pos] == '\n') {
                    std::string line = text.substr(lineStart, pos - lineStart);
                    float lw = measureTextWidthTTF(line);
                    if (lw > maxLineW) maxLineW = lw;
                    lineCount++;
                    lineStart = pos + 1;
                }
            }
            std::string lastLine = text.substr(lineStart);
            float lw = measureTextWidthTTF(lastLine);
            if (lw > maxLineW) maxLineW = lw;
        }

        int textW = (int)(maxLineW + 1);
        int lineH = (int)(FONT_SIZE * 1.3f);
        int textH = lineCount * lineH;

        int padding = 8;
        int cornerRadius = 8;
        int bubbleW = textW + padding * 2;
        int bubbleH = textH + padding * 2;
        int tailH = 10;

        // Position bubble above-right of sprite
        int bubbleX = spriteFbX + sprW / 2 + 4;
        int bubbleY = spriteFbY - sprH / 2 - bubbleH - tailH;

        // Clamp to screen
        if (bubbleX + bubbleW > fbW - 2) bubbleX = fbW - bubbleW - 2;
        if (bubbleX < 2) bubbleX = 2;
        if (bubbleY < 2) bubbleY = 2;

        // Draw bubble (white fill + gray outline, rounded)
        fillRoundedRect(fb, fbW, fbH, bubbleX, bubbleY, bubbleW, bubbleH,
                        cornerRadius, 255, 255, 255);
        drawRoundedRectOutline(fb, fbW, fbH, bubbleX, bubbleY, bubbleW, bubbleH,
                               cornerRadius, 180, 180, 180);

        // Draw tail (small triangle pointing down toward sprite)
        {
            int tailX = bubbleX + 12;
            for (int row = 0; row < tailH; row++) {
                int w = tailH - row;
                fillRectFb(fb, fbW, fbH, tailX, bubbleY + bubbleH - 1 + row, w, 1,
                         255, 255, 255);
                // Left edge of tail
                if (tailX >= 0 && tailX < fbW) {
                    int ty = bubbleY + bubbleH - 1 + row;
                    if (ty >= 0 && ty < fbH) {
                        uint8_t *dst = fb + (ty * fbW + tailX) * 3;
                        dst[0] = 180; dst[1] = 180; dst[2] = 180;
                    }
                }
                // Right edge of tail
                int rx = tailX + w;
                if (rx >= 0 && rx < fbW) {
                    int ty = bubbleY + bubbleH - 1 + row;
                    if (ty >= 0 && ty < fbH) {
                        uint8_t *dst = fb + (ty * fbW + rx) * 3;
                        dst[0] = 180; dst[1] = 180; dst[2] = 180;
                    }
                }
            }
        }

        // Draw text (black, anti-aliased via stb_truetype)
        ensureFontLoaded();
        if (g_font_loaded) {
            drawStringTTF(fb, fbW, fbH,
                          bubbleX + padding, bubbleY + padding,
                          text, 0, 0, 0);
        }
    }
}
