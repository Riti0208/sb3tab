#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <unordered_map>
#include "esp_http_client.h"

// Scratch stage dimensions
#define STAGE_W 480
#define STAGE_H 360

struct CostumePixels {
    uint8_t *rgba = nullptr;  // RGBA8888
    int w = 0;
    int h = 0;
    double rotCenterX = 0;
    double rotCenterY = 0;
};

class SWRenderer {
public:
    SWRenderer();
    ~SWRenderer();

    // Download and decode a costume PNG from Scratch CDN
    // storeAs: optional key name (if different from md5ext, e.g. original SVG name)
    bool loadCostume(const std::string &md5ext, double rotCenterX, double rotCenterY,
                     const std::string &storeAs = "");

    // Load costume from memory buffer (for .sb3 ZIP extraction)
    bool loadCostumeFromMemory(const std::string &name, const uint8_t *data, size_t len,
                                double rotCenterX, double rotCenterY);

    // Clear framebuffer to white
    void clear();

    // Draw a sprite onto the framebuffer
    void drawSprite(const std::string &costumeMd5ext, float x, float y,
                    float direction, float size, bool visible,
                    float ghostEffect, float brightnessEffect = 0.0f);

    // Draw stage backdrop (no rotation, fills entire stage)
    void drawBackdrop(const std::string &costumeMd5ext);

    // Pen layer
    void initPenLayer();
    void clearPenLayer();
    uint8_t *getPenLayer() { return penFb; }
    void compositePenLayer();
    void penLine(float x1, float y1, float x2, float y2,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness);
    void penDot(float x, float y,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness);
    void penStampSprite(const std::string &costumeMd5ext, float x, float y,
                        float direction, float size, float ghostEffect);

    // Encode framebuffer as JPEG, returns malloc'd buffer (caller must free)
    uint8_t *encodeJpeg(int quality, int *outSize);

    // Get raw framebuffer
    uint8_t *getFramebuffer() { return fb; }

private:
    uint8_t *fb;      // STAGE_W * STAGE_H * 3 (RGB888)
    uint8_t *penFb;   // STAGE_W * STAGE_H * 4 (RGBA8888) pen layer
    std::unordered_map<std::string, CostumePixels> costumes;

    void blitRGBA(const CostumePixels &cos, int dstX, int dstY,
                  float scale, float angleDeg, float alpha,
                  float brightness = 0.0f);
};
