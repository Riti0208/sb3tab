#include <render.hpp>
#include <speech_manager.hpp>
#include <unordered_map>
#include <window.hpp>
#include <windowing/headless/window.hpp>
#include <color.hpp>
#include <algorithm>
#include "speech_manager_headless.hpp"
#include "headless_fb.hpp"

Window *globalWindow = nullptr;
static SpeechManagerHeadless *speechManager = nullptr;

// Costume size callback (set by main.cpp for collision/bounce)
static bool (*s_costume_size_cb)(const char *name, int *w, int *h) = nullptr;

extern "C" void render_set_costume_size_callback(
    bool (*cb)(const char *name, int *w, int *h)
) {
    s_costume_size_cb = cb;
}

// Called by runtime when costumeImages lookup fails
extern "C" bool render_get_costume_size(const char *name, int *w, int *h) {
    if (s_costume_size_cb) return s_costume_size_cb(name, w, h);
    return false;
}

// Pen layer callbacks (set by main.cpp)
struct PenCallbacks {
    void (*init)() = nullptr;
    void (*clear)() = nullptr;
    void (*line)(float x1, float y1, float x2, float y2,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) = nullptr;
    void (*dot)(float x, float y,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) = nullptr;
    void (*stamp)(const char *costume, float x, float y,
                  float direction, float size, float ghost) = nullptr;
};
static PenCallbacks penCb;
static bool penInitialized = false;

// Called from main.cpp to register SWRenderer pen functions
extern "C" void render_set_pen_callbacks(
    void (*init_fn)(),
    void (*clear_fn)(),
    void (*line_fn)(float, float, float, float, uint8_t, uint8_t, uint8_t, uint8_t, float),
    void (*dot_fn)(float, float, uint8_t, uint8_t, uint8_t, uint8_t, float),
    void (*stamp_fn)(const char *, float, float, float, float, float)
) {
    penCb.init = init_fn;
    penCb.clear = clear_fn;
    penCb.line = line_fn;
    penCb.dot = dot_fn;
    penCb.stamp = stamp_fn;
}

bool Render::Init() {
    globalWindow = new WindowHeadless();
    globalWindow->init(0, 0, "");
    return true;
}

void Render::deInit() {
    if (globalWindow) {
        globalWindow->cleanup();
        delete globalWindow;
        globalWindow = nullptr;
    }
    destroySpeechManager();
}

void *Render::getRenderer() {
    return nullptr;
}

bool Render::createSpeechManager() {
    if (speechManager == nullptr) {
        speechManager = new SpeechManagerHeadless();
    }
    return true;
}

void Render::destroySpeechManager() {
    if (speechManager) {
        delete speechManager;
        speechManager = nullptr;
    }
}

SpeechManager *Render::getSpeechManager() {
    return speechManager;
}

void Render::beginFrame(int screen, int colorR, int colorG, int colorB) {
}

void Render::endFrame(bool shouldFlush) {
}

bool Render::initPen() {
    if (penInitialized) return true;
    if (penCb.init) {
        penCb.init();
        penInitialized = true;
        return true;
    }
    return false;
}

static void getPenColorAlpha(Sprite *sprite, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a) {
    ColorRGBA rgba = CSBT2RGBA(sprite->penData.color);
    r = (uint8_t)rgba.r;
    g = (uint8_t)rgba.g;
    b = (uint8_t)rgba.b;
    a = (uint8_t)((100.0 - sprite->penData.color.transparency) / 100.0 * 255.0);
}

void Render::penMoveFast(double x1, double y1, double x2, double y2, Sprite *sprite) {
    if (!penCb.line) return;
    uint8_t r, g, b, a;
    getPenColorAlpha(sprite, r, g, b, a);
    penCb.line((float)x1, (float)y1, (float)x2, (float)y2,
               r, g, b, a, (float)sprite->penData.size);
}

void Render::penDotFast(Sprite *sprite) {
    if (!penCb.dot) return;
    uint8_t r, g, b, a;
    getPenColorAlpha(sprite, r, g, b, a);
    penCb.dot((float)sprite->xPosition, (float)sprite->yPosition,
              r, g, b, a, (float)sprite->penData.size);
}

void Render::penMoveAccurate(double x1, double y1, double x2, double y2, Sprite *sprite) {
    penMoveFast(x1, y1, x2, y2, sprite);
}

void Render::penDotAccurate(Sprite *sprite) {
    penDotFast(sprite);
}

void Render::penStamp(Sprite *sprite) {
    if (!penCb.stamp) return;
    int cosIdx = sprite->currentCostume;
    if (cosIdx < 0 || cosIdx >= (int)sprite->costumes.size()) return;
    penCb.stamp(sprite->costumes[cosIdx].fullName.c_str(),
                (float)sprite->xPosition, (float)sprite->yPosition,
                (float)sprite->rotation, (float)sprite->size,
                sprite->ghostEffect);
}

void Render::penClear() {
    if (penCb.clear) penCb.clear();
}

int Render::getWidth() {
    return Scratch::projectWidth;
}

int Render::getHeight() {
    return Scratch::projectHeight;
}

void Render::renderSprites() {
}

// Defined in speech_manager_headless.cpp
extern void fillRectFb(uint8_t *fb, int fbW, int fbH,
                       int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b);

void Render::drawBox(int w, int h, int x, int y, uint8_t colorR, uint8_t colorG, uint8_t colorB, uint8_t colorA) {
    if (!g_headless_fb.fb || w <= 0 || h <= 0) return;
    // x,y is center of box in the original API
    int left = x - w / 2;
    int top = y - h / 2;
    if (colorA < 255) {
        // Simple alpha: skip if fully transparent
        if (colorA == 0) return;
    }
    int radius = 0;
    if (w >= 12 && h >= 12) {
        radius = std::min({3, w / 2, h / 2});
    }
    int fbW = g_headless_fb.width;
    int fbH = g_headless_fb.height;
    if (radius == 0) {
        fillRectFb(g_headless_fb.fb, fbW, fbH, left, top, w, h, colorR, colorG, colorB);
        return;
    }
    fillRectFb(g_headless_fb.fb, fbW, fbH, left, top + radius, w, h - 2 * radius,
               colorR, colorG, colorB);
    fillRectFb(g_headless_fb.fb, fbW, fbH, left + radius, top, w - 2 * radius, radius,
               colorR, colorG, colorB);
    fillRectFb(g_headless_fb.fb, fbW, fbH, left + radius, top + h - radius, w - 2 * radius, radius,
               colorR, colorG, colorB);
    int r2 = radius * radius;
    for (int cy = 0; cy < radius; cy++) {
        int dy = radius - cy;
        for (int cx = 0; cx < radius; cx++) {
            int dx = radius - cx;
            if (dx * dx + dy * dy > r2) continue;
            int px = left + cx, py = top + cy;
            if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
                uint8_t *d = g_headless_fb.fb + (py * fbW + px) * 3;
                d[0] = colorR; d[1] = colorG; d[2] = colorB;
            }
            px = left + w - 1 - cx;
            if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
                uint8_t *d = g_headless_fb.fb + (py * fbW + px) * 3;
                d[0] = colorR; d[1] = colorG; d[2] = colorB;
            }
            py = top + h - 1 - cy; px = left + cx;
            if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
                uint8_t *d = g_headless_fb.fb + (py * fbW + px) * 3;
                d[0] = colorR; d[1] = colorG; d[2] = colorB;
            }
            px = left + w - 1 - cx;
            if (px >= 0 && px < fbW && py >= 0 && py < fbH) {
                uint8_t *d = g_headless_fb.fb + (py * fbW + px) * 3;
                d[0] = colorR; d[1] = colorG; d[2] = colorB;
            }
        }
    }
}

bool Render::appShouldRun() {
    return true;
}
