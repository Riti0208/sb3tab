#include "lcd_driver.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "soc/gpio_reg.h"

static const char *TAG = "lcd";

esp_lcd_panel_handle_t lcd_init()
{
    // Backlight
    if (LCD_PIN_BL >= 0) {
        gpio_config_t bl_cfg = {};
        bl_cfg.pin_bit_mask = 1ULL << LCD_PIN_BL;
        bl_cfg.mode = GPIO_MODE_OUTPUT;
        gpio_config(&bl_cfg);
        gpio_set_level((gpio_num_t)LCD_PIN_BL, 1);
    }

    // SPI bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.sclk_io_num = LCD_PIN_SCLK;
    bus_cfg.mosi_io_num = LCD_PIN_MOSI;
    bus_cfg.miso_io_num = -1;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = LCD_W * 40 * 2;  // 40 lines at a time
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Panel IO (SPI)
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

    // ILI9341 panel driver
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_PIN_RST;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    // Landscape mode
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));

    // ESP32-P4 workaround: SPI driver sets GPIO Matrix routing but doesn't
    // enable GPIO output. Write directly to GPIO_ENABLE_W1TS to enable
    // output without disturbing GPIO Matrix OUT_SEL configuration.
    // G21(SCK), G19(CS) → GPIO_ENABLE_W1TS (GPIO 0-31)
    // G38(MOSI) → GPIO_ENABLE1_W1TS (GPIO 32-54, bit 38-32=6)
    // G35(DC) → GPIO_ENABLE1_W1TS (bit 35-32=3)
    // G37(RST) → GPIO_ENABLE1_W1TS (bit 37-32=5)
    REG_WRITE(GPIO_ENABLE_W1TS_REG, (1 << LCD_PIN_SCLK) | (1 << LCD_PIN_CS));
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, (1 << (LCD_PIN_MOSI - 32)) |
                                      (1 << (LCD_PIN_DC - 32)) |
                                      (1 << (LCD_PIN_RST - 32)));
    ESP_LOGI(TAG, "GPIO output enable set for SPI LCD pins (P4 workaround)");

    ESP_LOGI(TAG, "LCD initialized: %dx%d", LCD_W, LCD_H);
    return panel;
}

// DMA-capable buffer for SPI transfer
static uint16_t *dma_buf = nullptr;
static const int STRIP_H = 10;  // lines per transfer

void lcd_draw_framebuffer(esp_lcd_panel_handle_t panel, const uint16_t *fb)
{
    if (!dma_buf) {
        dma_buf = (uint16_t *)heap_caps_malloc(LCD_W * STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        if (!dma_buf) {
            dma_buf = (uint16_t *)heap_caps_malloc(LCD_W * STRIP_H * 2, MALLOC_CAP_DMA);
        }
        if (!dma_buf) {
            ESP_LOGE(TAG, "Failed to allocate DMA buffer");
            return;
        }
    }

    for (int y = 0; y < LCD_H; y += STRIP_H) {
        int h = STRIP_H;
        if (y + h > LCD_H) h = LCD_H - y;
        memcpy(dma_buf, fb + y * LCD_W, LCD_W * h * 2);
        esp_lcd_panel_draw_bitmap(panel, 0, y, LCD_W, y + h, dma_buf);
    }
}

// Nearest-neighbor scale from srcW x srcH RGB888 to dstW x dstH RGB565
void rgb888_to_rgb565_scaled(const uint8_t *src, int srcW, int srcH,
                             uint16_t *dst, int dstW, int dstH)
{
    for (int dy = 0; dy < dstH; dy++) {
        int sy = dy * srcH / dstH;
        const uint8_t *srcRow = src + sy * srcW * 3;
        uint16_t *dstRow = dst + dy * dstW;

        for (int dx = 0; dx < dstW; dx++) {
            int sx = dx * srcW / dstW;
            const uint8_t *p = srcRow + sx * 3;

            // RGB888 to RGB565 (big-endian for ILI9341)
            uint16_t r = p[0] >> 3;
            uint16_t g = p[1] >> 2;
            uint16_t b = p[2] >> 3;
            uint16_t c = (r << 11) | (g << 5) | b;
            // Swap bytes for SPI (ILI9341 expects big-endian)
            dstRow[dx] = (c >> 8) | (c << 8);
        }
    }
}
