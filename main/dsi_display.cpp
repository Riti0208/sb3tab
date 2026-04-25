// dsi_display.cpp - M5Stack Tab5 MIPI DSI display driver (ST7123)
// Initialization code based on M5Tab5-UserDemo BSP (m5stack_tab5.c)
// https://github.com/m5stack/M5Tab5-UserDemo

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dsi_display.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7123.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#include "sw_renderer.h"  // for STAGE_W, STAGE_H
#include <cstring>

static const char *TAG = "dsi_disp";

// ============================================================
// I2C GPIO expander — copied verbatim from BSP m5stack_tab5.c
// ============================================================

#define I2C_DEV_ADDR_PI4IOE1  0x43
#define I2C_DEV_ADDR_PI4IOE2  0x44
#define I2C_MASTER_TIMEOUT_MS 100
#define PI4IO_REG_CHIP_RESET  0x01
#define PI4IO_REG_IO_DIR      0x03
#define PI4IO_REG_OUT_SET     0x05
#define PI4IO_REG_OUT_H_IM    0x07
#define PI4IO_REG_IN_DEF_STA  0x09
#define PI4IO_REG_PULL_EN     0x0B
#define PI4IO_REG_PULL_SEL    0x0D
#define PI4IO_REG_INT_MASK    0x11

static i2c_master_dev_handle_t i2c_dev_handle_pi4ioe1 = NULL;
static i2c_master_dev_handle_t i2c_dev_handle_pi4ioe2 = NULL;

static void io_expander_init(i2c_master_bus_handle_t bus_handle)
{
    uint8_t write_buf[2] = {0};
    uint8_t read_buf[1]  = {0};

    // --- PI4IOE1 (0x43) ---
    i2c_device_config_t dev_cfg1 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = I2C_DEV_ADDR_PI4IOE1,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg1, &i2c_dev_handle_pi4ioe1));

    write_buf[0] = PI4IO_REG_CHIP_RESET; write_buf[1] = 0xFF;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(i2c_dev_handle_pi4ioe1, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_IO_DIR; write_buf[1] = 0b01111111;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_OUT_H_IM; write_buf[1] = 0b00000000;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_PULL_SEL; write_buf[1] = 0b01111111;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_PULL_EN; write_buf[1] = 0b01111111;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    // P1(SPK_EN), P2(EXT5V_EN), P4(LCD_RST), P5(TP_RST), P6(CAM_RST) = HIGH
    write_buf[0] = PI4IO_REG_OUT_SET; write_buf[1] = 0b01110110;
    i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);

    // --- PI4IOE2 (0x44) ---
    i2c_device_config_t dev_cfg2 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = I2C_DEV_ADDR_PI4IOE2,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg2, &i2c_dev_handle_pi4ioe2));

    write_buf[0] = PI4IO_REG_CHIP_RESET; write_buf[1] = 0xFF;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_CHIP_RESET;
    i2c_master_transmit_receive(i2c_dev_handle_pi4ioe2, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_IO_DIR; write_buf[1] = 0b10111001;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_OUT_H_IM; write_buf[1] = 0b00000110;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_PULL_SEL; write_buf[1] = 0b10111001;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_PULL_EN; write_buf[1] = 0b11111001;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_IN_DEF_STA; write_buf[1] = 0b01000000;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    write_buf[0] = PI4IO_REG_INT_MASK; write_buf[1] = 0b10111111;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
    // P0(WLAN_PWR_EN), P3(USB5V_EN), P7(CHG_EN) = HIGH
    write_buf[0] = PI4IO_REG_OUT_SET; write_buf[1] = 0b00001001;
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);

    ESP_LOGI(TAG, "PI4IOE expanders initialized");
}

// Charge QC enable (PI4IOE2 P5)
static void set_charge_qc_enable(bool en) {
    uint8_t write_buf[2], read_buf[1];
    write_buf[0] = PI4IO_REG_OUT_SET;
    i2c_master_transmit_receive(i2c_dev_handle_pi4ioe2, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
    write_buf[1] = read_buf[0];
    if (en) write_buf[1] &= ~(1 << 5); else write_buf[1] |= (1 << 5);
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
}

// Charge enable (PI4IOE2 P7)
static void set_charge_enable(bool en) {
    uint8_t write_buf[2], read_buf[1];
    write_buf[0] = PI4IO_REG_OUT_SET;
    i2c_master_transmit_receive(i2c_dev_handle_pi4ioe2, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
    write_buf[1] = read_buf[0];
    if (en) write_buf[1] |= (1 << 7); else write_buf[1] &= ~(1 << 7);
    i2c_master_transmit(i2c_dev_handle_pi4ioe2, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
}

// ============================================================
// Backlight
// ============================================================

#define LCD_LEDC_CH LEDC_CHANNEL_1

static void backlight_init() {
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t ch = {
        .gpio_num = DSI_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

static void backlight_set(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (4095 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH);
    ESP_LOGI(TAG, "Backlight set to %d%% (duty=%lu)", percent, duty);
}

// ============================================================
// DSI PHY power
// ============================================================

static void enable_dsi_phy_power() {
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&cfg, &ldo));
    ESP_LOGI(TAG, "MIPI DSI PHY powered (LDO3=2500mV)");
}

// ============================================================
// PPA + DPI framebuffer state
// ============================================================

static ppa_client_handle_t s_ppa_srm = NULL;
static uint16_t *s_dpi_fb = NULL;
static uint8_t *s_ppa_src_buf = NULL;          // Intermediate buffer for non-blocking PPA
static SemaphoreHandle_t s_ppa_done = NULL;     // Signaled when PPA SRM completes

static bool ppa_srm_done_cb(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done, &woken);
    return woken == pdTRUE;
}

// ============================================================
// Display init — ST7123 (Tab5 post-Oct 2025)
// ============================================================

esp_lcd_panel_handle_t dsi_display_init()
{
    // 1. I2C init
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_31,
        .scl_io_num = GPIO_NUM_32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = { .enable_internal_pullup = true },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    ESP_LOGI(TAG, "I2C bus initialized (SDA=31, SCL=32)");

    // 2. IO expander init
    io_expander_init(i2c_bus);

    // 3. Charge enable
    set_charge_qc_enable(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    set_charge_enable(true);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 3.5 Explicit LCD reset via IO expander (PI4IOE1 P4)
    {
        uint8_t write_buf[2], read_buf[1];
        // Read current OUT_SET, clear P4 (LCD_RST LOW)
        write_buf[0] = PI4IO_REG_OUT_SET;
        i2c_master_transmit_receive(i2c_dev_handle_pi4ioe1, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
        write_buf[1] = read_buf[0] & ~(1 << 4);
        i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
        ESP_LOGI(TAG, "LCD_RST LOW");
        vTaskDelay(pdMS_TO_TICKS(100));

        // Set P4 HIGH (LCD_RST HIGH)
        write_buf[0] = PI4IO_REG_OUT_SET;
        i2c_master_transmit_receive(i2c_dev_handle_pi4ioe1, write_buf, 1, read_buf, 1, I2C_MASTER_TIMEOUT_MS);
        write_buf[1] = read_buf[0] | (1 << 4);
        i2c_master_transmit(i2c_dev_handle_pi4ioe1, write_buf, 2, I2C_MASTER_TIMEOUT_MS);
        ESP_LOGI(TAG, "LCD_RST HIGH");
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // 4. Backlight init
    backlight_init();

    // 5. DSI PHY power
    enable_dsi_phy_power();

    // 6. DSI bus — ST7123: 2 lanes, 965 Mbps
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t dsi_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 965,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_cfg, &dsi_bus));

    // 7. DBI IO
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    // 8. DPI panel config — ST7123: 720x1280 portrait, 70MHz, RGB565
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = DSI_LCD_W,
            .v_size = DSI_LCD_H,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 8,
            .vsync_front_porch = 220,
        },
        .flags = { .use_dma2d = true },
    };

    // 9. ST7123 panel
    st7123_vendor_config_t vendor_cfg = {
        .init_cmds = NULL,      // use built-in defaults
        .init_cmds_size = 0,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
            .lane_num = 2,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(dbi_io, &dev_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "ST7123 panel initialized: %dx%d (24bpp, 70MHz, 965Mbps)", DSI_LCD_W, DSI_LCD_H);

    // 10. Backlight ON
    backlight_set(100);

    // 11. Initialize PPA SRM client with async callback
    {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ppa_srm));

        ppa_event_callbacks_t cbs = { .on_trans_done = ppa_srm_done_cb };
        ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_srm, &cbs));

        s_ppa_done = xSemaphoreCreateBinary();
        xSemaphoreGive(s_ppa_done);  // Pre-give so first frame doesn't block

        // Intermediate buffer: PPA reads from here while CPU renders next frame
        s_ppa_src_buf = (uint8_t *)heap_caps_malloc(
            STAGE_W * STAGE_H * 3, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "PPA SRM client registered (async, pipelined)");
    }

    // 12. Get DPI framebuffer pointer and clear to black
    {
        void *fb0 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) == ESP_OK && fb0) {
            s_dpi_fb = (uint16_t *)fb0;
            memset(fb0, 0, DSI_LCD_W * DSI_LCD_H * 2);
            esp_cache_msync(fb0, DSI_LCD_W * DSI_LCD_H * 2,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }

    return panel;
}

// ============================================================
// PPA SRM accelerated display update
// ============================================================

void dsi_display_update(esp_lcd_panel_handle_t panel,
                        const uint8_t *scratch_fb, int src_w, int src_h)
{
    if (!panel || !scratch_fb || !s_ppa_srm || !s_dpi_fb || !s_ppa_src_buf) return;

    // Wait for previous frame's PPA to finish (first frame: semaphore pre-given)
    xSemaphoreTake(s_ppa_done, pdMS_TO_TICKS(100));

    // Copy scratch FB to intermediate buffer so PPA can read while CPU renders next frame
    memcpy(s_ppa_src_buf, scratch_fb, src_w * src_h * 3);

    // PPA SRM: scale + rotate + color convert (non-blocking)
    ppa_srm_oper_config_t srm = {};

    srm.in.buffer = s_ppa_src_buf;
    srm.in.pic_w = src_w;
    srm.in.pic_h = src_h;
    srm.in.block_w = src_w;
    srm.in.block_h = src_h;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;

    srm.out.buffer = s_dpi_fb;
    srm.out.buffer_size = DSI_LCD_W * DSI_LCD_H * 2;
    srm.out.pic_w = DSI_LCD_W;
    srm.out.pic_h = DSI_LCD_H;
    srm.out.block_offset_x = 0;
    srm.out.block_offset_y = DSI_SCRATCH_X_OFFSET;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.scale_x = (float)DSI_SCRATCH_W / src_w;
    srm.scale_y = (float)DSI_SCRATCH_H / src_h;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    srm.mirror_x = false;
    srm.mirror_y = false;

    srm.rgb_swap = true;
    srm.byte_swap = false;
    srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    srm.mode = PPA_TRANS_MODE_NON_BLOCKING;  // Return immediately, callback signals completion

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa_srm, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_ppa_done);  // Unblock next frame
    }
}
