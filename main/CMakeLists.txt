idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_http_client
        esp_event
        nvs_flash
        esp_netif
        esp_psram
        mbedtls
        json
        scratch_core
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=all
    -include ${CMAKE_SOURCE_DIR}/components/scratch_core/deps/miniz/miniz.h
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIZ_HEADER_INCLUDED=1)


