#include "headless_fb.hpp"
#include <cstring>

HeadlessFB g_headless_fb;

// Defined in speech_manager_headless.cpp
extern bool g_font_loaded;
extern float g_font_scale;
extern void ensureFontLoaded();
extern void drawStringTTF(uint8_t *fb, int fbW, int fbH,
                          int px, int py, const std::string &text,
                          uint8_t r, uint8_t g, uint8_t b);
extern float measureTextWidthTTF(const std::string &text);
extern void fillRectFb(uint8_t *fb, int fbW, int fbH,
                       int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b);

static const float BASE_FONT_SIZE = 14.0f;

void headless_draw_string(int px, int py, const std::string &text,
                          uint8_t r, uint8_t g, uint8_t b, float scale) {
    if (!g_headless_fb.fb) return;
    ensureFontLoaded();
    if (!g_font_loaded) return;
    // stb_truetype renders at g_font_scale which corresponds to FONT_SIZE=14px
    // For different scales, we just use the base rendering (scale ~1.0 at 14px)
    drawStringTTF(g_headless_fb.fb, g_headless_fb.width, g_headless_fb.height,
                  px, py, text, r, g, b);
}

float headless_measure_text_width(const std::string &text, float scale) {
    ensureFontLoaded();
    if (!g_font_loaded) return text.size() * 7.0f;
    return measureTextWidthTTF(text);
}

float headless_get_text_height(float scale) {
    return BASE_FONT_SIZE;
}
