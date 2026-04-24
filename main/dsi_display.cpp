#include "dsi_display.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_ek79007.h"
#include "driver/ppa.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "dsi_disp";

// MIPI DSI PHY power (LDO_VO3 = 2.5V on ESP32-P4)
#define MIPI_DSI_PHY_LDO_CHAN       3
#define MIPI_DSI_PHY_LDO_MV        2500

// DSI bus config
#define MIPI_DSI_LANE_NUM           2
#define MIPI_DSI_LANE_BITRATE_MBPS  1000

// EK79007 timing for 800x480 @ ~60Hz
// Refresh = 30000000 / (800+10+100+100) / (480+1+20+10) = 30M / 1010 / 511 ≈ 58 Hz
#define DPI_CLK_MHZ    30
#define LCD_HSYNC      10
#define LCD_HBP        100
#define LCD_HFP        100
#define LCD_VSYNC      1
#define LCD_VBP        20
#define LCD_VFP        10

// Backlight and reset GPIOs (-1 = not connected)
#define PIN_BK_LIGHT   -1
#define PIN_LCD_RST    -1

// PPA client handles
static ppa_client_handle_t s_ppa_srm = nullptr;
static ppa_client_handle_t s_ppa_fill = nullptr;

// Scaled framebuffer (640x480 RGB888) in DMA-capable PSRAM
static uint8_t *s_scaled_buf = nullptr;
static size_t s_scaled_buf_size = 0;

esp_lcd_panel_handle_t dsi_display_init()
{
    // --- PHY power ---
    esp_ldo_channel_handle_t ldo_mipi_phy = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY powered on (LDO%d = %dmV)", MIPI_DSI_PHY_LDO_CHAN, MIPI_DSI_PHY_LDO_MV);

    // --- DSI bus ---
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = MIPI_DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = MIPI_DSI_LANE_BITRATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    // --- DBI command IO ---
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    // --- DPI data panel config ---
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB888,
        .video_timing = {
            .h_size = DSI_LCD_W,
            .v_size = DSI_LCD_H,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_back_porch = LCD_HBP,
            .hsync_front_porch = LCD_HFP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_back_porch = LCD_VBP,
            .vsync_front_porch = LCD_VFP,
        },
        .flags = {
            .use_dma2d = true,
        },
    };

    // --- EK79007 panel driver ---
    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };

    esp_lcd_panel_handle_t panel = nullptr;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &dev_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ESP_LOGI(TAG, "EK79007 panel initialized: %dx%d @ %dMHz", DSI_LCD_W, DSI_LCD_H, DPI_CLK_MHZ);

    // --- Backlight ---
#if PIN_BK_LIGHT >= 0
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level((gpio_num_t)PIN_BK_LIGHT, 1);
#endif

    // --- PPA clients ---
    ppa_client_config_t srm_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&srm_cfg, &s_ppa_srm));

    ppa_client_config_t fill_cfg = {
        .oper_type = PPA_OPERATION_FILL,
        .max_pending_trans_num = 1,
    };
    ESP_ERROR_CHECK(ppa_register_client(&fill_cfg, &s_ppa_fill));
    ESP_LOGI(TAG, "PPA clients registered (SRM + Fill)");

    // --- Allocate scaled framebuffer ---
    s_scaled_buf_size = DSI_SCRATCH_W * DSI_SCRATCH_H * 3;
    s_scaled_buf = (uint8_t *)heap_caps_calloc(s_scaled_buf_size, 1,
                                                MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!s_scaled_buf) {
        ESP_LOGE(TAG, "Failed to allocate scaled framebuffer (%zu bytes)", s_scaled_buf_size);
        return nullptr;
    }

    // --- Clear display to white ---
    // Fill the entire display via draw_bitmap with a white buffer
    // We reuse the scaled buffer (it's big enough for one strip at a time)
    memset(s_scaled_buf, 0xFF, s_scaled_buf_size);
    // Draw white strips to cover the full 800x480
    // Left letterbox
    if (DSI_SCRATCH_X_OFFSET > 0) {
        size_t strip_size = DSI_SCRATCH_X_OFFSET * DSI_LCD_H * 3;
        uint8_t *white_strip = (uint8_t *)heap_caps_malloc(strip_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        if (white_strip) {
            memset(white_strip, 0xFF, strip_size);
            esp_lcd_panel_draw_bitmap(panel, 0, 0, DSI_SCRATCH_X_OFFSET, DSI_LCD_H, white_strip);
            // Right letterbox
            esp_lcd_panel_draw_bitmap(panel, DSI_SCRATCH_X_OFFSET + DSI_SCRATCH_W, 0,
                                      DSI_LCD_W, DSI_LCD_H, white_strip);
            heap_caps_free(white_strip);
        }
    }

    return panel;
}

void dsi_display_update(esp_lcd_panel_handle_t panel,
                        const uint8_t *scratch_fb, int src_w, int src_h)
{
    if (!panel || !scratch_fb || !s_scaled_buf || !s_ppa_srm) return;

    float scale_x = (float)DSI_SCRATCH_W / src_w;
    float scale_y = (float)DSI_SCRATCH_H / src_h;

    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = scratch_fb;
    srm.in.pic_w = src_w;
    srm.in.pic_h = src_h;
    srm.in.block_w = src_w;
    srm.in.block_h = src_h;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;

    srm.out.buffer = s_scaled_buf;
    srm.out.buffer_size = s_scaled_buf_size;
    srm.out.pic_w = DSI_SCRATCH_W;
    srm.out.pic_h = DSI_SCRATCH_H;
    srm.out.block_offset_x = 0;
    srm.out.block_offset_y = 0;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;

    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm.scale_x = scale_x;
    srm.scale_y = scale_y;
    srm.rgb_swap = 0;
    srm.byte_swap = 0;
    srm.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_ppa_srm, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(ret));
        return;
    }

    // Draw scaled Scratch framebuffer centered on display
    esp_lcd_panel_draw_bitmap(panel,
                              DSI_SCRATCH_X_OFFSET, 0,
                              DSI_SCRATCH_X_OFFSET + DSI_SCRATCH_W, DSI_SCRATCH_H,
                              s_scaled_buf);
}
