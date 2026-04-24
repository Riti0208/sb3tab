#pragma once
#include <cstdint>
#include <string>

// Global framebuffer pointer for headless rendering (drawBox, text, monitors)
// Set by main.cpp before calling renderMonitors()
struct HeadlessFB {
    uint8_t *fb = nullptr;
    int width = 0;
    int height = 0;
};

extern HeadlessFB g_headless_fb;

// Draw text using stb_truetype onto the global framebuffer
void headless_draw_string(int px, int py, const std::string &text,
                          uint8_t r, uint8_t g, uint8_t b, float scale = 1.0f);
float headless_measure_text_width(const std::string &text, float scale = 1.0f);
float headless_get_text_height(float scale = 1.0f);
