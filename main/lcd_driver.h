#pragma once
#include "esp_lcd_panel_ops.h"
#include <cstdint>

// M5Stamp ESP32-P4 pin assignments for ILI9341 SPI LCD
// M5Stamp ESP32-P4 pin assignments for SPI LCD
// Confirmed working pins via LED test
#define LCD_PIN_SCLK    21
#define LCD_PIN_MOSI    38
#define LCD_PIN_CS      19
#define LCD_PIN_DC      35
#define LCD_PIN_RST     37
#define LCD_PIN_BL      -1  // backlight not connected

#define LCD_W  320
#define LCD_H  240
#define LCD_SPI_FREQ_HZ (20 * 1000 * 1000)

// Initialize the LCD display, returns panel handle
esp_lcd_panel_handle_t lcd_init();

// Send a full RGB565 framebuffer to the LCD (320x240)
void lcd_draw_framebuffer(esp_lcd_panel_handle_t panel, const uint16_t *fb);

// Convert RGB888 framebuffer (480x360) to RGB565 (320x240) with scaling
void rgb888_to_rgb565_scaled(const uint8_t *src, int srcW, int srcH,
                             uint16_t *dst, int dstW, int dstH);
