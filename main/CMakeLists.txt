idf_component_register(
    SRCS "main.cpp" "sw_renderer.cpp" "lcd_driver.cpp" "dsi_display.cpp" "esp_lcd_st7123.c"
         "wifi_manager.cpp" "camera_qr.cpp" "dsi_modal.cpp" "sd_storage.cpp"
         "input_device.cpp" "input_xbox.cpp" "input_gpio.cpp"
         "es8388_audio.cpp" "ui_menu.cpp" "i18n.cpp"
         "battery_monitor.cpp" "time_sync.cpp"
         "assets_gen/ui_logo.c"
         "assets_gen/ui_icon_play.c"
         "assets_gen/ui_icon_new.c"
         "assets_gen/ui_icon_settings.c"
    EMBED_FILES
         "assets/cursor.wav"
         "assets/select.wav"
         "assets/cancel.wav"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        driver
        esp_event
        nvs_flash
        esp_psram
        esp_timer
        hal
        scratch_core
        esp_driver_ppa
        esp_driver_ledc
        esp_driver_i2c
        esp_mm
        esp_wifi
        esp_netif
        esp_http_client
        esp_video
        quirc
        fatfs
        sdmmc
        usb
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=all
    # Restrict -include miniz.h to C/C++ so it doesn't apply to the auto-
    # generated .S files for EMBED_FILES (which would otherwise try to
    # preprocess miniz declarations as assembly).
    "$<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_SOURCE_DIR}/components/scratch_core/deps/miniz/miniz.h>"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIZ_HEADER_INCLUDED=1)

# Force-keep block registration constructors
target_link_libraries(${COMPONENT_LIB} PRIVATE
    "-Wl,--undefined=_GLOBAL__sub_I__Z17block_control_if_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z25block_data_setvariableto_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z22block_event_broadcast_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z16block_looks_say_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z23block_motion_movesteps_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z19block_operator_add_R5BlockP6Sprite"
    "-Wl,--undefined=_GLOBAL__sub_I__Z18block_pen_penDown_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z38block_argument_reporter_string_number_R5BlockP6Sprite"
    "-Wl,--undefined=_GLOBAL__sub_I__Z25block_sensing_resettimer_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z26block_sound_playuntildone_R5BlockP6SpritePbb"
    "-Wl,--undefined=_GLOBAL__sub_I__Z31block_text2speech_speakAndWait_R5BlockP6SpritePbb"
)
