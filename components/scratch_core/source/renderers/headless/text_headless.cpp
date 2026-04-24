#include "text_headless.hpp"
#include "headless_fb.hpp"

TextObjectHeadless::TextObjectHeadless(std::string txt, double posX, double posY, std::string fontPath)
    : TextObject(txt, posX, posY, fontPath) {
}

TextObjectHeadless::~TextObjectHeadless() {
}

void TextObjectHeadless::setText(std::string txt) {
    text = txt;
}

void TextObjectHeadless::render(int xPos, int yPos) {
    if (text.empty() || !g_headless_fb.fb) return;

    uint8_t r = (uint8_t)((color >> 24) & 0xFF);
    uint8_t g = (uint8_t)((color >> 16) & 0xFF);
    uint8_t b = (uint8_t)((color >> 8) & 0xFF);

    int drawX = xPos;
    int drawY = yPos;

    if (centerAligned) {
        float w = headless_measure_text_width(text, scale);
        float h = headless_get_text_height(scale);
        drawX = xPos - (int)(w / 2);
        drawY = yPos - (int)(h / 2);
    }

    headless_draw_string(drawX, drawY, text, r, g, b, scale);
}

std::vector<float> TextObjectHeadless::getSize() {
    if (text.empty()) return {0.0f, 0.0f};
    float w = headless_measure_text_width(text, scale);
    float h = headless_get_text_height(scale);
    return {w, h};
}
