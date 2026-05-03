// S3 LCD test - identical to original working S3 lcd_driver code
#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

// Original S3 pin assignments (XIAO ESP32-S3)
#define LCD_PIN_SCLK    7   // D8
#define LCD_PIN_MOSI    9   // D10
#define LCD_PIN_CS      1   // D0
#define LCD_PIN_DC      2   // D1
#define LCD_PIN_RST     3   // D2
#define LCD_PIN_BL      4   // D3

#define LCD_W  320
#define LCD_H  240
#define LCD_SPI_FREQ_HZ (40 * 1000 * 1000)

static const char *TAG = "lcd_test";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "S3 LCD Test - ST7789 driver");

    // Backlight ON
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << LCD_PIN_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level((gpio_num_t)LCD_PIN_BL, 1);

    // SPI bus (same as original)
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = LCD_PIN_SCLK;
    bus_cfg.mosi_io_num = LCD_PIN_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = LCD_W * 40 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Panel IO (same as original)
    esp_lcd_panel_io_handle_t io_handle = nullptr;
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.dc_gpio_num = LCD_PIN_DC;
    io_cfg.cs_gpio_num = LCD_PIN_CS;
    io_cfg.pclk_hz = LCD_SPI_FREQ_HZ;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    io_cfg.spi_mode = 0;
    io_cfg.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle));

    // ST7789 panel driver (original)
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_PIN_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Landscape mode (same as original)
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));

    ESP_LOGI(TAG, "LCD initialized, drawing red...");

    // Draw red screen (same DMA approach as original)
    uint16_t *dma_buf = (uint16_t *)heap_caps_malloc(LCD_W * 10 * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!dma_buf) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        return;
    }

    for (int i = 0; i < LCD_W * 10; i++) {
        uint16_t red = (0x1F << 11);
        dma_buf[i] = (red >> 8) | (red << 8);
    }

    for (int y = 0; y < LCD_H; y += 10) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + 10, dma_buf);
    }

    ESP_LOGI(TAG, "RED screen drawn!");

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
