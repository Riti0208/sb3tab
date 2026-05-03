#include "touch_input.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_st7123.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

extern i2c_master_bus_handle_t g_i2c_bus;

static const char *TAG = "touch";

// Panel: ST7123 720x1280 portrait. Tab5 is held landscape with the panel
// rotated 90° CW for display, so we apply the same rotation to touch coords:
//   landscape_x = panel_y
//   landscape_y = (PANEL_W - 1) - panel_x
#define TP_INT_GPIO       23
#define PANEL_W           720
#define PANEL_H           1280
#define LANDSCAPE_W       1280
#define LANDSCAPE_H       720

// Scratch stage rendered at 2x in the center of the landscape area
// (see DSI_SCRATCH_W / DSI_SCRATCH_X_OFFSET in dsi_display.h)
#define SCRATCH_X_OFFSET  160
#define STAGE_PIX_W       480
#define STAGE_PIX_H       360
#define STAGE_HALF_W      240
#define STAGE_HALF_H      180

static esp_lcd_touch_handle_t s_tp = NULL;
static SemaphoreHandle_t s_lock = NULL;
static touch_raw_t s_last = {};

static void touch_task(void *)
{
    uint16_t x[1], y[1], strength[1];
    uint8_t cnt = 0;
    while (true) {
        if (esp_lcd_touch_read_data(s_tp) == ESP_OK) {
            cnt = 0;
            bool pressed = esp_lcd_touch_get_coordinates(s_tp, x, y, strength, &cnt, 1);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (pressed && cnt > 0) {
                s_last.x = x[0];
                s_last.y = y[0];
                s_last.pressed = true;
            } else {
                s_last.pressed = false;
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(15));  // ~66 Hz
    }
}

extern "C" bool touch_input_init(void)
{
    if (!g_i2c_bus) {
        ESP_LOGE(TAG, "g_i2c_bus is null — call dsi_display_init() first");
        return false;
    }

    s_lock = xSemaphoreCreateMutex();

    // The header's ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG() macro uses a designator
    // order that doesn't match esp_lcd_panel_io_i2c_config_t's declaration
    // order, which trips -Werror=designator-order. Build the config manually
    // with the same values the macro sets (addr 0x55, 100kHz, 16-bit cmd,
    // disable control phase).
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {};
    io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS;
    io_cfg.scl_speed_hz = 100000;
    io_cfg.control_phase_bytes = 1;
    io_cfg.lcd_cmd_bits = 16;
    io_cfg.flags.disable_control_phase = 1;
    esp_err_t err = esp_lcd_new_panel_io_i2c(g_i2c_bus, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel_io_i2c err=0x%x", err);
        return false;
    }

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = PANEL_W;
    tp_cfg.y_max = PANEL_H;
    tp_cfg.rst_gpio_num = GPIO_NUM_NC;       // PI4IOE1 P5 already drove this HIGH at boot
    tp_cfg.int_gpio_num = (gpio_num_t)TP_INT_GPIO;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;

    err = esp_lcd_touch_new_i2c_st7123(io_handle, &tp_cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_new_st7123 err=0x%x", err);
        return false;
    }

    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "ST7123 touch initialized (INT=GPIO%d)", TP_INT_GPIO);
    return true;
}

extern "C" bool touch_input_get_raw(touch_raw_t *out)
{
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_last;
    xSemaphoreGive(s_lock);
    return true;
}

extern "C" bool touch_input_get_stage(touch_stage_t *out)
{
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pressed = s_last.pressed;
    int px = s_last.x;
    int py = s_last.y;
    xSemaphoreGive(s_lock);

    out->stage_x = 0;
    out->stage_y = 0;
    out->pressed = false;
    if (!pressed) return true;

    // Panel(720x1280 portrait) → landscape(1280x720). The DPI panel is rotated
    // 270° (PPA_SRM_ROTATION_ANGLE_270 in dsi_display.cpp), so:
    //   landscape_x = (PANEL_H - 1) - panel_y
    //   landscape_y = panel_x
    int lx = (PANEL_H - 1) - py;
    int ly = px;
    if (lx < 0) lx = 0;
    if (lx >= LANDSCAPE_W) lx = LANDSCAPE_W - 1;
    if (ly < 0) ly = 0;
    if (ly >= LANDSCAPE_H) ly = LANDSCAPE_H - 1;

    // Pillarbox: Scratch area is 960 px wide, centered (160px black bars on each side)
    // Stage pixel coords (480x360) are landscape coords scaled by 0.5
    float sx_pix = (lx - SCRATCH_X_OFFSET) * 0.5f;
    float sy_pix = ly * 0.5f;

    // Scratch coords: centered, Y up
    out->stage_x = (int)(sx_pix - STAGE_HALF_W + 0.5f);
    out->stage_y = (int)(STAGE_HALF_H - sy_pix + 0.5f);
    out->pressed = true;
    return true;
}

extern "C" esp_lcd_touch_handle_t touch_input_get_handle(void)
{
    return s_tp;
}
