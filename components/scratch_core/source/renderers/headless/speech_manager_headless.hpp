#pragma once
#include "speech_manager.hpp"
#include "speech_text_headless.hpp"
#include <cstdint>

class SpeechManagerHeadless : public SpeechManager {
  protected:
    double getCurrentTime() override;
    void createSpeechObject(Sprite *sprite, const std::string &message) override;

  public:
    SpeechManagerHeadless();
    ~SpeechManagerHeadless();

    void render(int offsetX = 0, int offsetY = 0) override;

    // Draw speech bubbles onto an RGB888 framebuffer (called from main render loop)
    void renderToFramebuffer(uint8_t *fb, int fbW, int fbH,
                             int projectW, int projectH);
};
