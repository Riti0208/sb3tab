#include "blockUtils.hpp"

BlockResult nopBlock(Block &block, Sprite *sprite, bool *withoutScreenRefresh, bool fromRepeat) {
    return BlockResult::CONTINUE;
}

// ESP32 workaround: -fdata-sections + --gc-sections strips static block registrations.
// Include all block files here so they share a single translation unit.
#ifdef __ESP32__
#include "control.cpp"
#include "coreExample.cpp"
#include "data.cpp"
#include "events.cpp"
#include "looks.cpp"
#include "makeymakey.cpp"
#include "motion.cpp"
#include "operators.cpp"
#include "pen.cpp"
#include "procedures.cpp"
#include "sensing.cpp"
#include "shadow.cpp"
#include "sound.cpp"
#include "text2speech.cpp"
#endif
