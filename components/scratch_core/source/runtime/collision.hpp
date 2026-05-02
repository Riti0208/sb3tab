#pragma once

#include "sprite.hpp"
#include <image.hpp>
#include <memory>

#if defined(__NDS__) || defined(__PSP__) || defined(GAMECUBE)
constexpr unsigned int bitmaskScaleFactor = 3;
#elif defined(__3DS__) || defined(WII)
constexpr unsigned int bitmaskScaleFactor = 2;
#elif defined(RENDERER_HEADLESS)
// Headless ESP32-P4: large SVG ground sprites generate ~200K bitmask cells at
// scale 1, which makes per-frame touching checks dominate the frame budget.
constexpr unsigned int bitmaskScaleFactor = 3;
#else
constexpr unsigned int bitmaskScaleFactor = 1;
#endif

namespace collision {
std::shared_ptr<Bitmask> generateBitmask(Sprite *sprite, unsigned int scaleFactor = bitmaskScaleFactor);
bool pointInSprite(Sprite *sprite, float x, float y);
bool spriteInSprite(Sprite *a, Sprite *b);
bool spriteOnEdge(Sprite *sprite);

bool pointInSpriteFast(Sprite *sprite, float x, float y);
bool spriteInSpriteFast(Sprite *a, Sprite *b);
bool spriteOnEdgeFast(Sprite *sprite);

// Color-touching: any opaque pixel of `self` overlaps a pixel of color (tr,tg,tb)
// from any other visible sprite or the stage backdrop.
bool spriteTouchingColor(Sprite *self, uint8_t tr, uint8_t tg, uint8_t tb);
// Color-on-self touches color: pixels of self matching (sr,sg,sb) overlap a
// pixel of color (tr,tg,tb) from any other visible sprite/stage.
bool colorTouchingColor(Sprite *self, uint8_t sr, uint8_t sg, uint8_t sb,
                        uint8_t tr, uint8_t tg, uint8_t tb);
} // namespace collision
