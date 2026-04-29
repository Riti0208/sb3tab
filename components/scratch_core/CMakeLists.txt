set(SCRATCH_SRCS
    source/runtime/blockExecutor.cpp
    # blockUtils.cpp #includes all other block .cpp files when __ESP32__ is defined
    # to prevent --gc-sections from stripping static block registrations
    source/runtime/blocks/blockUtils.cpp
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
    source/renderers/headless/speech_text_headless.cpp
    source/renderers/headless/speech_manager_headless.cpp
    source/renderers/headless/headless_fb.cpp
    source/windowing/headless/input.cpp
    source/audio/esp32/audio.cpp
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
        esp_timer
        esp_driver_i2s
        esp_http_client
        mbedtls
    EMBED_FILES
        gfx/ingame/fonts/NotoSansJP-Medium-subset.ttf
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    HEADLESS=1
    RENDERER_HEADLESS=1
    SE_HEADLESS=1
    __ESP32__=1
    MINIZ_NO_STDIO=1
    MINIZ_NO_TIME=1
    NDEBUG=1
    ENABLE_AUDIO=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# Prevent linker from stripping static block registrations (SCRATCH_BLOCK macros)
set(BLOCK_CTORS
    _GLOBAL__sub_I__Z17block_control_if_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z25block_data_setvariableto_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z22block_event_broadcast_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z16block_looks_say_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z23block_motion_movesteps_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z19block_operator_add_R5BlockP6Sprite
    _GLOBAL__sub_I__Z18block_pen_penDown_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z38block_argument_reporter_string_number_R5BlockP6Sprite
    _GLOBAL__sub_I__Z25block_sensing_resettimer_R5BlockP6SpritePbb
    _GLOBAL__sub_I__Z13block_matrix_R5BlockP6Sprite
    _GLOBAL__sub_I__Z26block_sound_playuntildone_R5BlockP6SpritePbb
)
foreach(SYM ${BLOCK_CTORS})
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--undefined=${SYM}")
endforeach()

# Disable section-level GC for blockUtils.cpp (contains all block registrations via #include)
# Force-include ScratchEverywhere's miniz.h for C++ files only
file(GLOB_RECURSE SCRATCH_CXX_SRCS "${COMPONENT_DIR}/source/*.cpp")
set_source_files_properties(${SCRATCH_CXX_SRCS}
    PROPERTIES
        COMPILE_OPTIONS "-include;${COMPONENT_DIR}/deps/miniz/miniz.h"
        COMPILE_DEFINITIONS "MINIZ_HEADER_INCLUDED=1"
)

# blockUtils.cpp includes all block files - disable section GC to keep static registrations
set_source_files_properties(${COMPONENT_DIR}/source/runtime/blocks/blockUtils.cpp
    PROPERTIES
        COMPILE_OPTIONS "-include;${COMPONENT_DIR}/deps/miniz/miniz.h;-fno-function-sections;-fno-data-sections"
        COMPILE_DEFINITIONS "MINIZ_HEADER_INCLUDED=1"
)

set_source_files_properties(
    ${COMPONENT_DIR}/deps/miniz/miniz.c
    PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_TIME"
)
set_source_files_properties(
    ${COMPONENT_DIR}/deps/ryujs/ryu/d2s.c
    PROPERTIES COMPILE_OPTIONS ""
)

# Use ScratchEverywhere's miniz (with ZIP support) instead of ESP-IDF's (which disables ZIP)
# Place it first in include path to override ESP-IDF's miniz.h
target_include_directories(${COMPONENT_LIB} BEFORE PUBLIC
    ${COMPONENT_DIR}/deps/miniz
)
