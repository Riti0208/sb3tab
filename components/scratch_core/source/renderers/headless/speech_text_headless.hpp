#pragma once
#include "text_headless.hpp"
#include <speech_text.hpp>
#include <string>

class SpeechTextObjectHeadless : public TextObjectHeadless, public SpeechText {
  private:
    float measureTextWidth(const std::string &text) override;
    void platformSetText(const std::string &text) override;

  public:
    SpeechTextObjectHeadless(const std::string &text, int maxWidth = 200);
    ~SpeechTextObjectHeadless() override;

    void setText(std::string txt) override;
    void render(int xPos, int yPos) override {}
    std::vector<float> getSize() override;
};
