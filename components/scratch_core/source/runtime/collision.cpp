#include "collision.hpp"
#include "image.hpp"
#include "math.hpp"
#include "os.hpp"
#include "runtime.hpp"
#include "sprite.hpp"
#include <cmath>
#include <cstdint>

// Weak callback: lets headless platforms expose costume RGBA pixel data without
// going through the Image / costumeImages cache. Returns nullptr if not provided.
extern "C" __attribute__((weak)) const uint8_t *render_get_costume_rgba(const char *name, int *w, int *h) {
    (void)name; (void)w; (void)h;
    return nullptr;
}

// Companion callback for render_get_costume_rgba: returns raster_pixels /
// logical_pixels. Defaults to 1.0 so non-headless platforms (where raster ==
// logical) keep working unchanged. Headless renderers that downscale SVGs or
// honour bitmapResolution=2 must override this; otherwise the bitmask's
// rotation center lands in the wrong cell and collision shifts.
extern "C" __attribute__((weak)) float render_get_costume_decode_scale(const char *name) {
    (void)name;
    return 1.0f;
}

std::shared_ptr<Bitmask> collision::generateBitmask(Sprite *sprite, unsigned int scaleFactor) {
    const auto &costume = sprite->costumes[sprite->currentCostume];

    ImageData imgData;
    bool imgDataValid = false;
    auto imgFind = Scratch::costumeImages.find(costume.fullName);
    if (imgFind != Scratch::costumeImages.end()) {
        imgData = imgFind->second->getPixels();
        imgDataValid = true;
    } else {
        // Headless fallback: pull RGBA directly from the platform renderer.
        int rw = 0, rh = 0;
        const uint8_t *rgba = render_get_costume_rgba(costume.fullName.c_str(), &rw, &rh);
        if (rgba && rw > 0 && rh > 0) {
            imgData.width = rw;
            imgData.height = rh;
            imgData.pixels = const_cast<uint8_t *>(rgba);
            imgData.pitch = rw * 4;
            // raster_size / logical_size. <1 for downscaled SVGs, ==bitmap-
            // Resolution for PNG. Drives bitmask scaleFactor so the rotation
            // center (in logical coords) maps to the correct cell.
            imgData.scale = render_get_costume_decode_scale(costume.fullName.c_str());
            if (imgData.scale <= 0.0f) imgData.scale = 1.0f;
            imgDataValid = true;
        }
    }
    if (!imgDataValid) {
        Log::logWarning("Failed to find image for sprite: " + sprite->name);
        return nullptr;
    }

    std::shared_ptr<Bitmask> bitmask = std::make_shared<Bitmask>();
    bitmask->width = imgData.width / scaleFactor;
    bitmask->height = imgData.height / scaleFactor;
    bitmask->scaleFactor = (float)scaleFactor / imgData.scale;
    const unsigned int rowWords = (bitmask->width + 31) / 32;
    bitmask->bits.resize(rowWords * bitmask->height, 0);

    float maxDistSq = 0;
    const float centerX = costume.rotationCenterX / bitmask->scaleFactor;
    const float centerY = costume.rotationCenterY / bitmask->scaleFactor;

    uint32_t *pixels = (uint32_t *)imgData.pixels;
    for (int y = 0; y < bitmask->height; y++) {
        for (int x = 0; x < bitmask->width; x++) {
            const uint32_t px = pixels[(y * scaleFactor) * imgData.width + (x * scaleFactor)];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            const uint8_t alpha = px & 0xFF;
#else
            const uint8_t alpha = (px >> 24) & 0xFF;
#endif
            if (alpha > 0) {
                bitmask->bits[y * rowWords + (x / 32)] |= (1 << (x % 32));

                const float dx = x - centerX;
                const float dy = y - centerY;
                const float distSq = dx * dx + dy * dy;
                if (distSq > maxDistSq) maxDistSq = distSq;
            }
        }
    }

    bitmask->maxRadius = std::sqrt(maxDistSq) * bitmask->scaleFactor;

    // they need the pixel data freed because they make new pixels instead of a reference
#if defined(RENDERER_CITRO2D) || defined(RENDERER_GL2D)
    free(imgData.pixels);
#endif

    return bitmask;
}

bool collision::pointInSprite(Sprite *sprite, float x, float y) {
    auto &costume = sprite->costumes[sprite->currentCostume];
    std::shared_ptr<Bitmask> bitmask = costume.bitmask;
    if (bitmask == nullptr) {
        bitmask = generateBitmask(sprite);
        if (bitmask == nullptr) return false;
    }

    const float dx = x - sprite->xPosition;
    const float dy = y - sprite->yPosition;
    const float distSq = dx * dx + dy * dy;

    const float scaledRadius = bitmask->maxRadius * (sprite->size / 100.0f);
    if (distSq > (scaledRadius * scaledRadius)) {
        return false;
    }

    const float rad = sprite->rotationStyle == Sprite::RotationStyle::ALL_AROUND ? Math::degreesToRadians(-(sprite->rotation - 90)) : 0;
    const float s_sin = std::sin(rad);
    const float s_cos = std::cos(rad);

    const float localX = (dx * s_cos - (-dy) * s_sin) / (sprite->size / 100.0f);
    const float localY = (dx * s_sin + (-dy) * s_cos) / (sprite->size / 100.0f);

    const float invertedScaleFactor = 1.0f / bitmask->scaleFactor;
    float finalX = std::round((localX + costume.rotationCenterX) * invertedScaleFactor);
    const float finalY = std::round((localY + costume.rotationCenterY) * invertedScaleFactor);

    if (sprite->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) finalX = bitmask->width - finalX;

    return bitmask->getPixel(finalX, finalY);
}

#ifdef RENDERER_HEADLESS
#include "esp_timer.h"
// Small fingerprint-keyed cache for touching results within a frame.
// Many projects query `touching X` multiple times per frame from boolean args
// and inline conditions; positions/costumes only change at motion blocks, so
// a fingerprint match means the prior result is still valid. ~9 distinct pairs
// in the Ninja Action demo, so 16 entries leaves headroom.
namespace {
struct TouchCacheEntry {
    Sprite *a;
    Sprite *b;
    float ax, ay, bx, by;
    int ac, bc;
    float ad, bd;     // direction (LEFT_RIGHT flips bitmask access)
    float as, bs;     // size
    uint8_t ars, brs; // rotationStyle (changes are rare but supported)
    bool result;
    bool valid;
};
constexpr int TOUCH_CACHE_SIZE = 16;
TouchCacheEntry s_touch_cache[TOUCH_CACHE_SIZE];
int s_touch_cache_idx = 0;
}

// Measurement counters (reset/dumped from main.cpp once per frame)
namespace collision {
int g_call_count = 0;
int g_cache_hit = 0;
int g_inner_iters = 0;
int64_t g_total_us = 0;
}

// C-linkage accessors for measurement from main.cpp
extern "C" int collision_g_call_count() { return collision::g_call_count; }
extern "C" int collision_g_cache_hit() { return collision::g_cache_hit; }
extern "C" int collision_g_inner_iters() { return collision::g_inner_iters; }
extern "C" int64_t collision_g_total_us() { return collision::g_total_us; }
extern "C" void collision_reset_counters() {
    collision::g_call_count = 0;
    collision::g_cache_hit = 0;
    collision::g_inner_iters = 0;
    collision::g_total_us = 0;
}
#endif

bool collision::spriteInSprite(Sprite *a, Sprite *b) {
    if (a == b) return false;

#ifdef RENDERER_HEADLESS
    g_call_count++;
    int64_t t_start = esp_timer_get_time();
    for (int i = 0; i < TOUCH_CACHE_SIZE; i++) {
        const TouchCacheEntry &e = s_touch_cache[i];
        if (!e.valid) continue;
        if (e.a == a && e.b == b &&
            e.ax == a->xPosition && e.ay == a->yPosition &&
            e.bx == b->xPosition && e.by == b->yPosition &&
            e.ac == a->currentCostume && e.bc == b->currentCostume &&
            e.ad == a->rotation && e.bd == b->rotation &&
            e.as == a->size && e.bs == b->size &&
            e.ars == (uint8_t)a->rotationStyle && e.brs == (uint8_t)b->rotationStyle) {
            g_cache_hit++;
            g_total_us += esp_timer_get_time() - t_start;
            return e.result;
        }
    }
#endif

    bool result = false;
    auto storeCache = [&]() {
#ifdef RENDERER_HEADLESS
        TouchCacheEntry &slot = s_touch_cache[s_touch_cache_idx];
        slot.a = a; slot.b = b;
        slot.ax = a->xPosition; slot.ay = a->yPosition;
        slot.bx = b->xPosition; slot.by = b->yPosition;
        slot.ac = a->currentCostume; slot.bc = b->currentCostume;
        slot.ad = a->rotation; slot.bd = b->rotation;
        slot.as = a->size; slot.bs = b->size;
        slot.ars = (uint8_t)a->rotationStyle; slot.brs = (uint8_t)b->rotationStyle;
        slot.result = result;
        slot.valid = true;
        s_touch_cache_idx = (s_touch_cache_idx + 1) % TOUCH_CACHE_SIZE;
        g_total_us += esp_timer_get_time() - t_start;
#endif
    };

    auto &costumeA = a->costumes[a->currentCostume];
    std::shared_ptr<Bitmask> bitmaskA = costumeA.bitmask;
    if (bitmaskA == nullptr) {
        if (costumeA.bitmaskGenFailed) return false;
        bitmaskA = generateBitmask(a);
        if (bitmaskA == nullptr) {
            costumeA.bitmaskGenFailed = true;
            return false;
        }
        costumeA.bitmask = bitmaskA;
    }

    auto &costumeB = b->costumes[b->currentCostume];
    std::shared_ptr<Bitmask> bitmaskB = costumeB.bitmask;
    if (bitmaskB == nullptr) {
        if (costumeB.bitmaskGenFailed) return false;
        bitmaskB = generateBitmask(b);
        if (bitmaskB == nullptr) {
            costumeB.bitmaskGenFailed = true;
            return false;
        }
        costumeB.bitmask = bitmaskB;
    }

    const float dx = a->xPosition - b->xPosition;
    const float dy = a->yPosition - b->yPosition;
    const float distSq = dx * dx + dy * dy;

    const float radiusA = bitmaskA->maxRadius * (a->size / 100.0f);
    const float radiusB = bitmaskB->maxRadius * (b->size / 100.0f);
    const float combinedRadius = radiusA + radiusB;

    if (distSq > (combinedRadius * combinedRadius)) {
        storeCache();
        return false;
    }

    const float overlapMinX = std::max(a->xPosition - radiusA, b->xPosition - radiusB);
    const float overlapMaxX = std::min(a->xPosition + radiusA, b->xPosition + radiusB);
    const float overlapMinY = std::max(a->yPosition - radiusA, b->yPosition - radiusB);
    const float overlapMaxY = std::min(a->yPosition + radiusA, b->yPosition + radiusB);

    if (overlapMinX > overlapMaxX || overlapMinY > overlapMaxY) {
        storeCache();
        return false;
    }

    const float radA = a->rotationStyle == Sprite::RotationStyle::ALL_AROUND ? Math::degreesToRadians(-(a->rotation - 90)) : 0;
    const float sinA = std::sin(radA);
    const float cosA = std::cos(radA);
    const float spriteScaleA = a->size / 100.0f;
    const float invScaleA = (1.0f / bitmaskA->scaleFactor);

    const float radB = b->rotationStyle == Sprite::RotationStyle::ALL_AROUND ? Math::degreesToRadians(-(b->rotation - 90)) : 0;
    const float sinB = std::sin(radB);
    const float cosB = std::cos(radB);
    const float spriteScaleB = b->size / 100.0f;
    const float invScaleB = (1.0f / bitmaskB->scaleFactor);

#ifdef RENDERER_HEADLESS
    // ESP32-P4 has limited float perf and pixel-perfect collision was the
    // dominant per-frame cost. Step in stage coords matches the bitmask cell
    // size (scaleFactor) — single-bit checks cover scaleFactor² stage pixels
    // anyway, so finer stride is wasted work.
    const float strideA = std::max(1.0f, bitmaskA->scaleFactor);
    const float strideB = std::max(1.0f, bitmaskB->scaleFactor);
    const float stride = std::min(strideA, strideB);
#else
    const float stride = 1.0f;
#endif

    // Fast path: neither sprite has effective rotation. sin=0, cos=1, so
    // local coords are just translated/scaled — no per-pixel mul/div.
    // Triggers when rotationStyle is left-right/none, or when ALL_AROUND but
    // facing direction=90 (Scratch default) — the latter is the common case
    // for ground/obstacle sprites that never actually rotate.
    const float kRotEps = 1e-4f;
    const bool fastA = (std::fabs(sinA) < kRotEps && cosA > 1.0f - kRotEps);
    const bool fastB = (std::fabs(sinB) < kRotEps && cosB > 1.0f - kRotEps);

    if (fastA && fastB) {
        const float fA = invScaleA / spriteScaleA;     // costume_pixels per stage_pixel / scaleFactor
        const float fB = invScaleB / spriteScaleB;
        const float cxA = costumeA.rotationCenterX * invScaleA;
        const float cyA = costumeA.rotationCenterY * invScaleA;
        const float cxB = costumeB.rotationCenterX * invScaleB;
        const float cyB = costumeB.rotationCenterY * invScaleB;
        const float bmAw = bitmaskA->width;
        const float bmBw = bitmaskB->width;
        const bool flipA = a->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT;
        const bool flipB = b->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT;

        const float radSqA = radiusA * radiusA;
        const float radSqB = radiusB * radiusB;
        const float ax = a->xPosition, ay = a->yPosition;
        const float bx = b->xPosition, by = b->yPosition;

        for (float y = overlapMinY; y <= overlapMaxY; y += stride) {
            const float dyA = y - ay;
            const float dyB = y - by;
            const float dyA_sq = dyA * dyA;
            const float dyB_sq = dyB * dyB;
            const int finalYA_int = (int)((-dyA) * fA + cyA + 0.5f);
            const int finalYB_int = (int)((-dyB) * fB + cyB + 0.5f);

            for (float x = overlapMinX; x <= overlapMaxX; x += stride) {
#ifdef RENDERER_HEADLESS
                g_inner_iters++;
#endif
                const float dxA = x - ax;
                if (dxA * dxA + dyA_sq > radSqA) continue;

                int finalXA = (int)(dxA * fA + cxA + 0.5f);
                if (flipA) finalXA = (int)bmAw - finalXA;
                if (!bitmaskA->getPixel(finalXA, finalYA_int)) continue;

                const float dxB = x - bx;
                if (dxB * dxB + dyB_sq > radSqB) continue;

                int finalXB = (int)(dxB * fB + cxB + 0.5f);
                if (flipB) finalXB = (int)bmBw - finalXB;
                if (bitmaskB->getPixel(finalXB, finalYB_int)) {
                    result = true;
                    storeCache();
                    return true;
                }
            }
        }
    } else {
        for (float y = overlapMinY; y <= overlapMaxY; y += stride) {
            for (float x = overlapMinX; x <= overlapMaxX; x += stride) {
#ifdef RENDERER_HEADLESS
                g_inner_iters++;
#endif
                const float dxA = x - a->xPosition;
                const float dyA = y - a->yPosition;

                if ((dxA * dxA + dyA * dyA) > (radiusA * radiusA)) continue;

                const float localXA = (dxA * cosA - (-dyA) * sinA) / spriteScaleA;
                const float localYA = (dxA * sinA + (-dyA) * cosA) / spriteScaleA;
                float finalXA = std::round((localXA + costumeA.rotationCenterX) * invScaleA);
                const float finalYA = std::round((localYA + costumeA.rotationCenterY) * invScaleA);

                if (a->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) finalXA = bitmaskA->width - finalXA;

                if (!bitmaskA->getPixel(finalXA, finalYA)) continue;

                const float dxB = x - b->xPosition;
                const float dyB = y - b->yPosition;

                if ((dxB * dxB + dyB * dyB) > (radiusB * radiusB)) continue;

                const float localXB = (dxB * cosB - (-dyB) * sinB) / spriteScaleB;
                const float localYB = (dxB * sinB + (-dyB) * cosB) / spriteScaleB;
                float finalXB = std::round((localXB + costumeB.rotationCenterX) * invScaleB);
                const float finalYB = std::round((localYB + costumeB.rotationCenterY) * invScaleB);

                if (b->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) finalXB = bitmaskB->width - finalXB;

                if (bitmaskB->getPixel(finalXB, finalYB)) {
                    result = true;
                    storeCache();
                    return true;
                }
            }
        }
    }

    storeCache();
    return false;
}

bool collision::spriteOnEdge(Sprite *sprite) {
    auto &costume = sprite->costumes[sprite->currentCostume];
    std::shared_ptr<Bitmask> bitmask = costume.bitmask;
    if (bitmask == nullptr) {
        bitmask = generateBitmask(sprite);
        if (bitmask == nullptr) return false;
    }

    const float halfWidth = Scratch::projectWidth / 2.0f;
    const float halfHeight = Scratch::projectHeight / 2.0f;

    const float scaledRadius = bitmask->maxRadius * (sprite->size / 100.0f);

    if (sprite->xPosition - scaledRadius > -halfWidth &&
        sprite->xPosition + scaledRadius < halfWidth &&
        sprite->yPosition - scaledRadius > -halfHeight &&
        sprite->yPosition + scaledRadius < halfHeight) {
        return false;
    }

    const float rad = sprite->rotationStyle == Sprite::RotationStyle::ALL_AROUND ? Math::degreesToRadians(-(sprite->rotation - 90)) : 0;
    const float s_sin = std::sin(rad);
    const float s_cos = std::cos(rad);
    const float spriteScale = sprite->size / 100.0f;
    const float invScale = 1.0f / bitmask->scaleFactor;

    const float minX = std::floor(sprite->xPosition - scaledRadius);
    const float maxX = std::ceil(sprite->xPosition + scaledRadius);
    const float minY = std::floor(sprite->yPosition - scaledRadius);
    const float maxY = std::ceil(sprite->yPosition + scaledRadius);

    for (float y = minY; y <= maxY; y++) {
        for (float x = minX; x <= maxX; x++) {
            if (x > -halfWidth && x < halfWidth && y > -halfHeight && y < halfHeight) continue;

            const float dx = x - sprite->xPosition;
            const float dy = y - sprite->yPosition;

            if ((dx * dx + dy * dy) > (scaledRadius * scaledRadius)) continue;

            const float localX = (dx * s_cos - (-dy) * s_sin) / spriteScale;
            const float localY = (dx * s_sin + (-dy) * s_cos) / spriteScale;

            float finalX = std::round((localX + costume.rotationCenterX) * invScale);
            const float finalY = std::round((localY + costume.rotationCenterY) * invScale);

            if (sprite->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) finalX = bitmask->width - finalX;

            if (bitmask->getPixel(finalX, finalY)) {
                return true;
            }
        }
    }

    return false;
}

struct AABB {
    float left, right, top, bottom;
};

static inline AABB getSpriteBounds(Sprite *sprite) {
    float x = sprite->xPosition;
    float y = sprite->yPosition;

    float scale = sprite->size * 0.01f;

    const Costume &cos = sprite->costumes[sprite->currentCostume];
    int rotCenterX = cos.rotationCenterX;
    int rotCenterY = cos.rotationCenterY;

    // Use trimmed (visible-pixel) bounds when available so SVG padding doesn't
    // inflate the hitbox; fall back to full image when not yet computed.
    int boxOffX = 0, boxOffY = 0;
    int boxW = sprite->spriteWidth;
    int boxH = sprite->spriteHeight;
    if (cos.trimWidth > 0 && cos.trimHeight > 0) {
        boxOffX = cos.trimOffsetX;
        boxOffY = cos.trimOffsetY;
        boxW = cos.trimWidth;
        boxH = cos.trimHeight;
    }

    // Center of the (possibly trimmed) box in costume pixel coords.
    float boxCenterX = boxOffX + boxW / 2.0f;
    float boxCenterY = boxOffY + boxH / 2.0f;

    // Offset from rotation center to box center, in stage units (Y flipped).
    float offsetX = (boxCenterX - rotCenterX) * scale;
    float offsetY = (rotCenterY - boxCenterY) * scale;

    float finalX = x + offsetX;
    float finalY = y + offsetY;

    float halfW = (boxW * scale) / 2.0f;
    float halfH = (boxH * scale) / 2.0f;

    return {
        finalX - halfW,
        finalX + halfW,
        finalY + halfH,
        finalY - halfH};
}

bool collision::pointInSpriteFast(Sprite *sprite, float x, float y) {
    AABB box = getSpriteBounds(sprite);

    return (x >= box.left && x <= box.right &&
            y >= box.bottom && y <= box.top);
}

bool collision::spriteInSpriteFast(Sprite *a, Sprite *b) {
    AABB boxA = getSpriteBounds(a);
    AABB boxB = getSpriteBounds(b);

    return (boxA.left <= boxB.right) &&
           (boxA.right >= boxB.left) &&
           (boxA.bottom <= boxB.top) &&
           (boxA.top >= boxB.bottom);
}

bool collision::spriteOnEdgeFast(Sprite *sprite) {
    AABB box = getSpriteBounds(sprite);

    const float rightEdge = Scratch::projectWidth / 2.0f;
    const float leftEdge = -rightEdge;
    const float topEdge = Scratch::projectHeight / 2.0f;
    const float bottomEdge = -topEdge;

    return (box.right >= rightEdge) ||
           (box.left <= leftEdge) ||
           (box.top >= topEdge) ||
           (box.bottom <= bottomEdge);
}

// =====================================================================
// Color-touching support (sensing_touchingcolor / sensing_coloristouchingcolor)
// =====================================================================
// Headless renderer can't read back composited framebuffer pixels (and even
// if it could, layer-order would hide pixels of "lower" sprites). Instead we
// sample each candidate sprite's costume RGBA directly via the platform-
// supplied weak callback used by the bitmask generator.

extern "C" __attribute__((weak)) const uint8_t *render_get_costume_rgba(const char *name, int *w, int *h);

namespace {

// Tolerance per channel. SVG rasterization + scaling produces solid-fill
// pixels that drift a few units from the picker's color. ±12 catches the
// game's flat-color ground/lava pixels through anti-aliasing without
// matching unrelated shaded regions.
inline bool colorMatches(uint8_t ar, uint8_t ag, uint8_t ab,
                         uint8_t br, uint8_t bg, uint8_t bb) {
    auto diff = [](uint8_t a, uint8_t b) -> int {
        return a > b ? (int)(a - b) : (int)(b - a);
    };
    return diff(ar, br) <= 2 && diff(ag, bg) <= 2 && diff(ab, bb) <= 2;
}

// Pre-fetched sampling state for a sprite/stage. Builds the costume→stage
// transform constants once so per-pixel sampling is a few muls and a bounds
// check — no mutex, no name lookups, no trig.
struct SampleCtx {
    Sprite *sprite;
    const uint8_t *rgba;
    int w, h;
    bool isStage;
    bool flipH;     // LEFT_RIGHT rotation style
    // For sprites: fx = (dx*cs - (-dy)*sn)/scale + rotCx; fy = (dx*sn + (-dy)*cs)/scale + rotCy
    float ax, ay;   // sprite center (xPosition, yPosition)
    float cs, sn;   // cos/sin of effective rotation
    float invScale; // 1 / (size/100)
    float rotCx, rotCy;
    // Stage-coord AABB used for fast overlap pre-filtering. For stage this
    // covers the whole project area.
    float bboxL, bboxR, bboxB, bboxT;
};

bool prepareSampleCtx(Sprite *s, SampleCtx &ctx) {
    ctx.sprite = s;
    ctx.rgba = nullptr;
    if (!s || s->costumes.empty()) return false;
    const Costume &cos = s->costumes[s->currentCostume];

    int w = 0, h = 0;
    const uint8_t *rgba = render_get_costume_rgba ? render_get_costume_rgba(cos.fullName.c_str(), &w, &h) : nullptr;
    if (!rgba) {
        auto it = Scratch::costumeImages.find(cos.fullName);
        if (it == Scratch::costumeImages.end()) return false;
        ImageData id = it->second->getPixels();
        if (!id.pixels || id.width <= 0 || id.height <= 0) return false;
        rgba = (const uint8_t *)id.pixels;
        w = id.width;
        h = id.height;
    }
    ctx.rgba = rgba;
    ctx.w = w;
    ctx.h = h;
    ctx.isStage = s->isStage;
    ctx.flipH = (s->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT);
    ctx.rotCx = (float)cos.rotationCenterX;
    ctx.rotCy = (float)cos.rotationCenterY;

    if (s->isStage) {
        // Stage backdrop: drawn scaled-to-fill the stage area, so rotCenter
        // from JSON is irrelevant. Map stage→costume linearly using image
        // dimensions vs project size. Stash the scale into the existing
        // ctx fields by repurposing rotCx/rotCy/invScale; sampleAt has a
        // dedicated stage branch that consumes them.
        ctx.ax = ctx.ay = 0;
        ctx.cs = 1; ctx.sn = 0;
        ctx.invScale = 1;
        ctx.rotCx = (float)w / Scratch::projectWidth;   // stage→costume X scale
        ctx.rotCy = (float)h / Scratch::projectHeight;  // stage→costume Y scale
        ctx.bboxL = -Scratch::projectWidth / 2.0f;
        ctx.bboxR = Scratch::projectWidth / 2.0f;
        ctx.bboxB = -Scratch::projectHeight / 2.0f;
        ctx.bboxT = Scratch::projectHeight / 2.0f;
    } else {
        const float scale = s->size / 100.0f;
        if (scale <= 0) { ctx.rgba = nullptr; return false; }
        ctx.invScale = 1.0f / scale;
        ctx.ax = s->xPosition;
        ctx.ay = s->yPosition;
        if (s->rotationStyle == Sprite::RotationStyle::ALL_AROUND) {
            const float rad = Math::degreesToRadians(-(s->rotation - 90));
            ctx.cs = std::cos(rad);
            ctx.sn = std::sin(rad);
        } else {
            ctx.cs = 1;
            ctx.sn = 0;
        }
        AABB box = getSpriteBounds(s);
        ctx.bboxL = box.left;
        ctx.bboxR = box.right;
        ctx.bboxB = box.bottom;
        ctx.bboxT = box.top;
    }
    return true;
}

// Sample pre-prepared sprite at stage coord (sx, sy). Hot path: no locks,
// no string lookups. RGB out only valid when return == true.
inline bool sampleAt(const SampleCtx &c, float sx, float sy,
                     uint8_t &r, uint8_t &g, uint8_t &b) {
    float fx, fy;
    if (c.isStage) {
        // rotCx/rotCy hold the stage→costume scale (set in prepareSampleCtx).
        fx = (sx + Scratch::projectWidth / 2.0f) * c.rotCx;
        fy = (Scratch::projectHeight / 2.0f - sy) * c.rotCy;
    } else {
        const float dx = sx - c.ax;
        const float dy = sy - c.ay;
        const float lx = (dx * c.cs + dy * c.sn) * c.invScale;
        const float ly = (dx * c.sn - dy * c.cs) * c.invScale;
        fx = lx + c.rotCx;
        fy = ly + c.rotCy;
        if (c.flipH) fx = (float)c.w - fx;
    }
    const int ix = (int)(fx + 0.5f);
    const int iy = (int)(fy + 0.5f);
    if ((unsigned)ix >= (unsigned)c.w || (unsigned)iy >= (unsigned)c.h) return false;

    const uint32_t px = ((const uint32_t *)c.rgba)[iy * c.w + ix];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if ((px & 0xFF) == 0) return false;
    r = (px >> 24) & 0xFF;
    g = (px >> 16) & 0xFF;
    b = (px >> 8) & 0xFF;
#else
    if (((px >> 24) & 0xFF) == 0) return false;
    r = px & 0xFF;
    g = (px >> 8) & 0xFF;
    b = (px >> 16) & 0xFF;
#endif
    return true;
}

} // namespace

static constexpr int kMaxOthers = 32;

namespace {
inline bool aabbOverlap(const SampleCtx &a, float l, float r, float b, float t) {
    return !(a.bboxR < l || a.bboxL > r || a.bboxT < b || a.bboxB > t);
}

// Prepare only "other" sprites whose AABB overlaps `self`'s AABB. Cuts the
// per-pixel sample work by ~3-5× in typical games where most sprites are
// nowhere near the player. Stage backdrop is included only if explicitly
// referenced — most touching-color targets are sprite pixels.
int prepareOthers(Sprite *self, const SampleCtx &selfCtx, SampleCtx *out) {
    int n = 0;
    for (Sprite *o : Scratch::sprites) {
        if (o == self) continue;
        if (!o->isStage && !o->visible) continue;
        if (n >= kMaxOthers) break;

        // AABB pre-filter using cheap float math from sprite state — skips
        // the expensive prepareSampleCtx (mutex + LRU + costume cache lookup)
        // for sprites that can't possibly overlap self's bounding box.
        if (!o->isStage) {
            AABB box = getSpriteBounds(o);
            if (box.right < selfCtx.bboxL || box.left > selfCtx.bboxR ||
                box.top < selfCtx.bboxB || box.bottom > selfCtx.bboxT) continue;
        }

        if (!prepareSampleCtx(o, out[n])) continue;
        n++;
    }
    return n;
}

inline int anyOtherHasColor(const SampleCtx *others, int n,
                            float sx, float sy,
                            uint8_t tr, uint8_t tg, uint8_t tb) {
    uint8_t r, g, b;
    for (int i = 0; i < n; i++) {
        const SampleCtx &c = others[i];
        if (sx < c.bboxL || sx > c.bboxR || sy < c.bboxB || sy > c.bboxT) continue;
        if (!sampleAt(c, sx, sy, r, g, b)) continue;
        if (colorMatches(r, g, b, tr, tg, tb)) return i;
    }
    return -1;
}
} // namespace

bool collision::spriteTouchingColor(Sprite *self, uint8_t tr, uint8_t tg, uint8_t tb) {
    if (!self || !self->visible || self->costumes.empty()) return false;
    SampleCtx selfCtx;
    if (!prepareSampleCtx(self, selfCtx)) return false;
    SampleCtx others[kMaxOthers];
    const int nOthers = prepareOthers(self, selfCtx, others);

    if (nOthers == 0) return false;

    // Coarse 5×5 grid over self's AABB. Trades fine-edge sensitivity for
    // predictable per-call cost (~100us) so script timing isn't disrupted.
    const float w = selfCtx.bboxR - selfCtx.bboxL;
    const float h = selfCtx.bboxT - selfCtx.bboxB;
    uint8_t sr, sg, sb;
    for (int gy = 0; gy < 5; gy++) {
        float y = selfCtx.bboxB + h * (gy + 0.5f) / 5.0f;
        for (int gx = 0; gx < 5; gx++) {
            float x = selfCtx.bboxL + w * (gx + 0.5f) / 5.0f;
            if (!sampleAt(selfCtx, x, y, sr, sg, sb)) continue;
            if (anyOtherHasColor(others, nOthers, x, y, tr, tg, tb) >= 0) return true;
        }
    }
    return false;
}

bool collision::colorTouchingColor(Sprite *self, uint8_t sr_t, uint8_t sg_t, uint8_t sb_t,
                                   uint8_t tr, uint8_t tg, uint8_t tb) {
    if (!self || !self->visible || self->costumes.empty()) return false;
    SampleCtx selfCtx;
    if (!prepareSampleCtx(self, selfCtx)) return false;
    SampleCtx others[kMaxOthers];
    const int nOthers = prepareOthers(self, selfCtx, others);
    if (nOthers == 0) return false;

    const float w = selfCtx.bboxR - selfCtx.bboxL;
    const float h = selfCtx.bboxT - selfCtx.bboxB;
    uint8_t sr, sg, sb;
    for (int gy = 0; gy < 5; gy++) {
        float y = selfCtx.bboxB + h * (gy + 0.5f) / 5.0f;
        for (int gx = 0; gx < 5; gx++) {
            float x = selfCtx.bboxL + w * (gx + 0.5f) / 5.0f;
            if (!sampleAt(selfCtx, x, y, sr, sg, sb)) continue;
            if (!colorMatches(sr, sg, sb, sr_t, sg_t, sb_t)) continue;
            if (anyOtherHasColor(others, nOthers, x, y, tr, tg, tb) >= 0) return true;
        }
    }
    return false;
}

