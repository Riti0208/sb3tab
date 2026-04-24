#include "speech_text_headless.hpp"

// Use the same stb_truetype globals from speech_manager_headless.cpp
extern bool g_font_loaded;
extern float g_font_scale;

// Forward: decode UTF-8 codepoint
static uint32_t decodeUtf8Local(const std::string &s, size_t &i) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80)       { cp = c;           extra = 0; }
    else if (c < 0xC0)  { cp = 0xFFFD;      extra = 0; }
    else if (c < 0xE0)  { cp = c & 0x1F;    extra = 1; }
    else if (c < 0xF0)  { cp = c & 0x0F;    extra = 2; }
    else                 { cp = c & 0x07;    extra = 3; }
    i++;
    for (int j = 0; j < extra && i < s.size(); j++, i++) {
        cp = (cp << 6) | ((uint8_t)s[i] & 0x3F);
    }
    return cp;
}

SpeechTextObjectHeadless::SpeechTextObjectHeadless(const std::string &text, int maxWidth)
    : TextObjectHeadless(text, 0, 0), SpeechText(text, maxWidth) {
    this->text = wrapText();
}

SpeechTextObjectHeadless::~SpeechTextObjectHeadless() {
}

float SpeechTextObjectHeadless::measureTextWidth(const std::string &txt) {
    // Approximate: 8px per ASCII char, 14px per multibyte (CJK) char
    // This is used for text wrapping decisions
    float w = 0;
    size_t i = 0;
    while (i < txt.size()) {
        uint32_t cp = decodeUtf8Local(txt, i);
        if (cp < 0x80) {
            w += 7.0f; // ~half of FONT_SIZE for ASCII
        } else {
            w += 14.0f; // full width for CJK
        }
    }
    return w;
}

void SpeechTextObjectHeadless::platformSetText(const std::string &txt) {
    this->text = txt;
}

void SpeechTextObjectHeadless::setText(std::string txt) {
    SpeechText::setText(txt);
}

std::vector<float> SpeechTextObjectHeadless::getSize() {
    float w = measureTextWidth(text);
    int lines = 1;
    for (char c : text) {
        if (c == '\n') lines++;
    }
    float h = lines * 14.0f * 1.3f;
    return {w, h};
}
