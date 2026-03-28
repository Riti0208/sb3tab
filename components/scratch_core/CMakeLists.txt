set(SCRATCH_SRCS
    source/runtime/blockExecutor.cpp
    source/runtime/blocks/blockUtils.cpp
    source/runtime/blocks/control.cpp
    source/runtime/blocks/coreExample.cpp
    source/runtime/blocks/data.cpp
    source/runtime/blocks/events.cpp
    source/runtime/blocks/looks.cpp
    source/runtime/blocks/makeymakey.cpp
    source/runtime/blocks/motion.cpp
    source/runtime/blocks/operators.cpp
    source/runtime/blocks/pen.cpp
    source/runtime/blocks/procedures.cpp
    source/runtime/blocks/sensing.cpp
    source/runtime/blocks/shadow.cpp
    source/runtime/blocks/sound.cpp
    source/runtime/blocks/text2speech.cpp
    source/runtime/collision.cpp
    source/runtime/math.cpp
    source/runtime/parser.cpp
    source/runtime/runtime.cpp
    source/runtime/time.cpp
    source/runtime/unzip.cpp
    source/runtime/value.cpp
    source/os.cpp
    source/render.cpp
    source/image.cpp
    source/input.cpp
    source/text.cpp
    source/settings.cpp
    source/speech_manager.cpp
    source/speech_text.cpp
    source/renderers/headless/image_headless.cpp
    source/renderers/headless/render.cpp
    source/renderers/headless/text_headless.cpp
    source/windowing/headless/input.cpp
    source/audio/headless/audio.cpp
    deps/miniz/miniz.c
    deps/miniz/miniz_zip.c
    deps/miniz/miniz_tinfl.c
    deps/miniz/miniz_tdef.c
    deps/ryujs/ryu/d2s.c
)

idf_component_register(
    SRCS ${SCRATCH_SRCS}
    INCLUDE_DIRS
        source
        source/runtime
        source/runtime/blocks
        source/renderers/headless
        source/windowing/headless
        source/audio/headless
        deps
        deps/ryujs
    REQUIRES
        newlib
        esp_rom
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    HEADLESS=1
    RENDERER_HEADLESS=1
    SE_HEADLESS=1
    __ESP32__=1
    MINIZ_NO_STDIO=1
    MINIZ_NO_TIME=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# Force-include ScratchEverywhere's miniz.h for C++ files only
# miniz.c compiles itself, so it doesn't need force-include
set_source_files_properties(
    ${COMPONENT_DIR}/deps/miniz/miniz.c
    PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_TIME"
)
set_source_files_properties(
    ${COMPONENT_DIR}/deps/ryujs/ryu/d2s.c
    PROPERTIES COMPILE_OPTIONS ""
)

# For all other source files, force-include miniz.h and block ESP-IDF's version
file(GLOB_RECURSE SCRATCH_CXX_SRCS "${COMPONENT_DIR}/source/*.cpp")
set_source_files_properties(${SCRATCH_CXX_SRCS}
    PROPERTIES
        COMPILE_OPTIONS "-include;${COMPONENT_DIR}/deps/miniz/miniz.h"
        COMPILE_DEFINITIONS "MINIZ_HEADER_INCLUDED=1"
)

# Use ScratchEverywhere's miniz (with ZIP support) instead of ESP-IDF's (which disables ZIP)
# Place it first in include path to override ESP-IDF's miniz.h
target_include_directories(${COMPONENT_LIB} BEFORE PUBLIC
    ${COMPONENT_DIR}/deps/miniz
)
