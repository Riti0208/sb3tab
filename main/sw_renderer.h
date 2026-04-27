#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
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

    // Clear framebuffer to white (full clear)
    void clear();

    // Clear only dirty rects from previous frame (for white backdrop)
    // Returns false if full clear is needed (first frame or too many dirty rects)
    bool clearDirty();

    // Draw a sprite onto the framebuffer
    void drawSprite(const std::string &costumeMd5ext, float x, float y,
                    float direction, float size, bool visible,
                    float ghostEffect, float brightnessEffect = 0.0f,
                    bool flipH = false);

    // Draw stage backdrop (no rotation, fills entire stage)
    void drawBackdrop(const std::string &costumeMd5ext);

    // Draw backdrop filling every pixel (replaces clear+drawBackdrop)
    // Returns false if costume not found (caller should clear instead)
    bool drawBackdropFull(const std::string &costumeMd5ext);

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

    // Get costume dimensions (returns false if not found)
    bool getCostumeSize(const std::string &name, int &w, int &h) const;

    // Encode framebuffer as JPEG, returns malloc'd buffer (caller must free)
    uint8_t *encodeJpeg(int quality, int *outSize);

    // Get raw framebuffer (current draw target)
    uint8_t *getFramebuffer() { return fb[currentFb]; }

    // Swap framebuffers: returns the completed frame's buffer for display
    uint8_t *swapFramebuffer() {
        uint8_t *completed = fb[currentFb];
        currentFb ^= 1;
        return completed;
    }

    // Notify a dirty region was drawn (called internally and by external renderers)
    void addDirtyRect(int x, int y, int w, int h);

private:
    uint8_t *fb[2];   // Double-buffered STAGE_W * STAGE_H * 3 (RGB888)
    int currentFb = 0;
    uint8_t *penFb;   // STAGE_W * STAGE_H * 4 (RGBA8888) pen layer
    std::unordered_map<std::string, CostumePixels> costumes;

    struct DirtyRect { int16_t x, y, w, h; };
    std::vector<DirtyRect> dirtyRects[2];  // per framebuffer
    bool needFullClear[2] = {true, true};

    void blitRGBA(const CostumePixels &cos, int dstX, int dstY,
                  float scale, float angleDeg, float alpha,
                  float brightness = 0.0f, bool flipH = false);
};
