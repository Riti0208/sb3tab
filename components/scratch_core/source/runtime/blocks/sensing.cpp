#include "blockUtils.hpp"
#include "collision.hpp"
#include "color.hpp"
#include <cmath>
#include <input.hpp>
#include <sprite.hpp>
#include <utility>
#include <value.hpp>
#include <vector>

SCRATCH_BLOCK(sensing, resettimer) {
    BlockExecutor::timer.start();
    return BlockResult::CONTINUE;
}

SCRATCH_BLOCK(sensing, askandwait) {
    const Value inputValue = Scratch::getInputValue(block, "QUESTION", sprite);
    Scratch::answer = Input::openSoftwareKeyboard(inputValue.asString().c_str());
    return BlockResult::CONTINUE;
}

SCRATCH_BLOCK(sensing, setdragmode) {
    const std::string mode = Scratch::getFieldValue(block, "DRAG_MODE");

    if (mode == "draggable") {
        sprite->draggable = true;
    } else if (mode == "not draggable") {
        sprite->draggable = false;
    }

    return BlockResult::CONTINUE;
}

SCRATCH_REPORTER_BLOCK(sensing, timer) {
    return Value(BlockExecutor::timer.getTimeMs() / 1000.0);
}

SCRATCH_REPORTER_BLOCK(sensing, of) {
    const std::string value = Scratch::getFieldValue(block, "PROPERTY");
    const Value inputValue = Scratch::getInputValue(block, "OBJECT", sprite);

    Sprite *spriteObject = nullptr;
    for (Sprite *currentSprite : Scratch::sprites) {
        if ((currentSprite->name == inputValue.asString() || (inputValue.asString() == "_stage_" && currentSprite->isStage)) && !currentSprite->isClone) {
            spriteObject = currentSprite;
            break;
        }
    }

    if (!spriteObject) return Value(0);

    if (spriteObject->isStage) {
        if (value == "background #") return Value(spriteObject->currentCostume + 1);
        if (value == "backdrop #") return Value(spriteObject->currentCostume + 1);
        if (value == "backdrop name") return Value(spriteObject->costumes[spriteObject->currentCostume].name);
    } else {
        if (value == "x position") return Value(spriteObject->xPosition);
        if (value == "y position") return Value(spriteObject->yPosition);
        if (value == "direction") return Value(spriteObject->rotation);
        if (value == "costume #") return Value(spriteObject->currentCostume + 1);
        if (value == "costume name") return Value(spriteObject->costumes[spriteObject->currentCostume].name);
        if (value == "backdrop name") return Value(spriteObject->costumes[spriteObject->currentCostume].name);
        if (value == "size") return Value(spriteObject->size);
    }

    for (const auto &[id, variable] : spriteObject->variables) {
        if (value == variable.name) return variable.value;
    }
    return Value(0);
}

SCRATCH_REPORTER_BLOCK(sensing, mousex) {
    return Value(Input::mousePointer.x);
}

SCRATCH_REPORTER_BLOCK(sensing, mousey) {
    return Value(Input::mousePointer.y);
}

SCRATCH_REPORTER_BLOCK(sensing, distanceto) {
    const Value inputValue = Scratch::getInputValue(block, "DISTANCETOMENU", sprite);

    if (inputValue.asString() == "_mouse_") {
        const double dx = Input::mousePointer.x - sprite->xPosition;
        const double dy = Input::mousePointer.y - sprite->yPosition;
        return Value(std::sqrt(dx * dx + dy * dy));
    }

    for (Sprite *currentSprite : Scratch::sprites) {
        if (currentSprite->name != inputValue.asString() || currentSprite->isClone) continue;
        const double dx = currentSprite->xPosition - sprite->xPosition;
        const double dy = currentSprite->yPosition - sprite->yPosition;
        return Value(std::sqrt(dx * dx + dy * dy));
    }

    return Value(10000);
}

SCRATCH_REPORTER_BLOCK(sensing, dayssince2000) {
    return Value(Time::getDaysSince2000());
}

SCRATCH_REPORTER_BLOCK(sensing, current) {
    std::string inputValue = Scratch::getFieldValue(block, "CURRENTMENU");

    if (inputValue == "YEAR") return Value(Time::getYear());
    if (inputValue == "MONTH") return Value(Time::getMonth());
    if (inputValue == "DATE") return Value(Time::getDay());
    if (inputValue == "DAYOFWEEK") return Value(Time::getDayOfWeek());
    if (inputValue == "HOUR") return Value(Time::getHours());
    if (inputValue == "MINUTE") return Value(Time::getMinutes());
    if (inputValue == "SECOND") return Value(Time::getSeconds());

    return Value();
}

SCRATCH_REPORTER_BLOCK(sensing, answer) {
    return Value(Scratch::answer);
}

SCRATCH_REPORTER_BLOCK(sensing, keypressed) {
    const Value keyName = Scratch::getInputValue(block, "KEY_OPTION", sprite);
    for (std::string button : Input::inputButtons) {
        if (Input::convertToKey(keyName) == button) return Value(true);
    }

    return Value(false);
}

SCRATCH_REPORTER_BLOCK(sensing, touchingobject) {
    const Value inputValue = Scratch::getInputValue(block, "TOUCHINGOBJECTMENU", sprite);

    if (inputValue.asString() == "_mouse_") return Value(Scratch::isColliding("mouse", sprite));
    if (inputValue.asString() == "_edge_") return Value(Scratch::isColliding("edge", sprite));

    for (size_t i = 0; i < Scratch::sprites.size(); i++) {
        Sprite *currentSprite = Scratch::sprites[i];
        if (currentSprite == sprite) continue;
        if (currentSprite->name == inputValue.asString() &&
            Scratch::isColliding("sprite", sprite, currentSprite, inputValue.asString())) {
            return Value(true);
        }
    }
    return Value(false);
}

SCRATCH_REPORTER_BLOCK(sensing, mousedown) {
    return Value(Input::mousePointer.isPressed);
}

namespace {
inline void colorValueToRGB(const Value &v, uint8_t &r, uint8_t &g, uint8_t &b) {
    const ColorRGBA rgba = CSBT2RGBA(v.asColor());
    auto clamp8 = [](float x) -> uint8_t {
        if (x < 0) x = 0; if (x > 255) x = 255;
        return (uint8_t)(x + 0.5f);
    };
    r = clamp8(rgba.r);
    g = clamp8(rgba.g);
    b = clamp8(rgba.b);
}
} // namespace

// Color-touching: pixel-perfect sampling has measurable per-call cost
// (mutex + per-other-sprite costume RGBA fetch + grid sample) that drops
// the headless build from ~25 fps to ~15 fps even on projects that don't
// depend on it. Most projects use sprite-level touching (sensing_touchingobject)
// for their gameplay collisions and only fall through to color as a hint;
// for those, returning false here is a fine approximation. Define
// ENABLE_COLOR_TOUCHING at build time when running a project that genuinely
// relies on color hit-testing (e.g. classic "touch ground color" platformers).
#ifdef ENABLE_COLOR_TOUCHING
SCRATCH_REPORTER_BLOCK(sensing, touchingcolor) {
    uint8_t r, g, b;
    colorValueToRGB(Scratch::getInputValue(block, "COLOR", sprite), r, g, b);
    return Value(collision::spriteTouchingColor(sprite, r, g, b));
}

SCRATCH_REPORTER_BLOCK(sensing, coloristouchingcolor) {
    uint8_t r1, g1, b1, r2, g2, b2;
    colorValueToRGB(Scratch::getInputValue(block, "COLOR", sprite), r1, g1, b1);
    colorValueToRGB(Scratch::getInputValue(block, "COLOR2", sprite), r2, g2, b2);
    return Value(collision::colorTouchingColor(sprite, r1, g1, b1, r2, g2, b2));
}
#else
SCRATCH_REPORTER_BLOCK(sensing, touchingcolor) {
    (void)block; (void)sprite;
    return Value(false);
}

SCRATCH_REPORTER_BLOCK(sensing, coloristouchingcolor) {
    (void)block; (void)sprite;
    return Value(false);
}
#endif

SCRATCH_REPORTER_BLOCK(sensing, username) {
#ifdef ENABLE_CLOUDVARS
    if (Scratch::cloudProject) return Value(Scratch::cloudUsername);
#endif
    if (Scratch::useCustomUsername) return Value(Scratch::customUsername);
    return Value(OS::getUsername());
}

SCRATCH_REPORTER_BLOCK(sensing, online) {
    return Value(OS::isOnline());
}

SCRATCH_REPORTER_BLOCK(sensing, userid) {
    return Value(Undefined{});
}
