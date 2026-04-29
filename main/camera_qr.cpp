// Camera + QR code scanner for M5Stack Tab5
// Camera: SC202CS via MIPI CSI (1280x720 RGB565)
// QR decoder: quirc
// Preview: PPA SRM rotation 270° to DPI framebuffer (720x1280)

#include "camera_qr.h"
#include "dsi_display.h"
#include "dsi_modal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/i2c_master.h"
#include "driver/ppa.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <cstring>
#include "quirc.h"
#include "dsi_modal.h"
#include "esp_video_isp_ioctl.h"

static const char *TAG = "cam_qr";

#define CAM_W  1280
#define CAM_H  720
#define CAM_BUF_COUNT  2

// V4L2 camera state
static int s_cam_fd = -1;
static uint8_t *s_cam_buffers[CAM_BUF_COUNT] = {};
static size_t s_cam_buf_sizes[CAM_BUF_COUNT] = {};

// QR decoder
static struct quirc *s_qr = nullptr;

// Grayscale conversion buffer (downscaled for QR: 640x360)
#define QR_W  640
#define QR_H  360
static uint8_t *s_gray_buf = nullptr;

// PPA for camera preview
static ppa_client_handle_t s_cam_ppa = nullptr;

// Strips reserved for static overlays (in landscape pixels). Configured by
// camera_set_preview_strips(); PPA SRM output is shrunk to skip them.
static int s_strip_top    = 0;
static int s_strip_bottom = 0;

void camera_set_preview_strips(int top_h, int bottom_h)
{
    if (top_h    < 0) top_h    = 0;
    if (bottom_h < 0) bottom_h = 0;
    s_strip_top    = top_h;
    s_strip_bottom = bottom_h;
}

// ============================================================
// RGB565 to grayscale (2x downscale for QR speed)
// ============================================================

static void rgb565_to_gray_half(const uint16_t *src, int src_w, int src_h,
                                 uint8_t *dst, int dst_w, int dst_h)
{
    for (int y = 0; y < dst_h; y++) {
        const uint16_t *row = src + (y * 2) * src_w;
        for (int x = 0; x < dst_w; x++) {
            uint16_t px = row[x * 2];
            int r = (px >> 11) & 0x1F;
            int g = (px >> 5) & 0x3F;
            int b = px & 0x1F;
            // BT.601 luma approximation for 5/6/5 bit
            dst[y * dst_w + x] = (uint8_t)((r * 19 + g * 9 + b * 5) >> 2);
        }
    }
}

// ============================================================
// Camera init via esp_video V4L2
// ============================================================

bool camera_init()
{
    if (s_cam_fd >= 0) return true;  // Already initialized

    // I2C bus for SCCB (sensor control) is already initialized in dsi_display_init()
    extern i2c_master_bus_handle_t g_i2c_bus;

    esp_video_init_csi_config_t csi_cfg = {};
    csi_cfg.sccb_config.init_sccb = false;
    csi_cfg.sccb_config.i2c_handle = g_i2c_bus;
    csi_cfg.sccb_config.freq = 400000;
    csi_cfg.reset_pin = (gpio_num_t)-1;
    csi_cfg.pwdn_pin = (gpio_num_t)-1;
    csi_cfg.dont_init_ldo = true;  // LDO3 already initialized by DSI display

    esp_video_init_config_t vid_cfg = {};
    vid_cfg.csi = &csi_cfg;

    esp_err_t ret = esp_video_init_with_flags(&vid_cfg,
        ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Open camera device
    s_cam_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDWR);
    if (s_cam_fd < 0) {
        ESP_LOGE(TAG, "Failed to open camera device");
        return false;
    }

    // Get current format first, then set desired
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam_fd, VIDIOC_G_FMT, &fmt) < 0) {
        ESP_LOGW(TAG, "VIDIOC_G_FMT failed, continuing");
    } else {
        ESP_LOGI(TAG, "DBG: current fmt: %lux%lu pix=0x%lx",
                 (unsigned long)fmt.fmt.pix.width, (unsigned long)fmt.fmt.pix.height,
                 (unsigned long)fmt.fmt.pix.pixelformat);
    }
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = CAM_W;
    fmt.fmt.pix.height = CAM_H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(s_cam_fd, VIDIOC_S_FMT, &fmt) < 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return false;
    }
    ESP_LOGI(TAG, "Camera format: %lux%lu RGB565",
             (unsigned long)fmt.fmt.pix.width, (unsigned long)fmt.fmt.pix.height);

    // Request buffers
    ESP_LOGI(TAG, "DBG: REQBUFS...");
    struct v4l2_requestbuffers req = {};
    req.count = CAM_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam_fd, VIDIOC_REQBUFS, &req) < 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return false;
    }
    ESP_LOGI(TAG, "DBG: REQBUFS OK, count=%lu", (unsigned long)req.count);

    // Map buffers
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_LOGI(TAG, "DBG: QUERYBUF %d...", i);
        if (ioctl(s_cam_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed for buf %d", i);
            close(s_cam_fd);
            s_cam_fd = -1;
            return false;
        }
        ESP_LOGI(TAG, "DBG: mmap buf %d, len=%lu, offset=%lu", i,
                 (unsigned long)buf.length, (unsigned long)buf.m.offset);
        s_cam_buffers[i] = (uint8_t *)mmap(NULL, buf.length,
                                            PROT_READ | PROT_WRITE, MAP_SHARED,
                                            s_cam_fd, buf.m.offset);
        s_cam_buf_sizes[i] = buf.length;
        ESP_LOGI(TAG, "DBG: mmap %d -> %p", i, s_cam_buffers[i]);

        // Queue buffer
        if (ioctl(s_cam_fd, VIDIOC_QBUF, &buf) < 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed for buf %d", i);
        }
    }

    // Start streaming
    ESP_LOGI(TAG, "DBG: STREAMON...");
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam_fd, VIDIOC_STREAMON, &type) < 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return false;
    }
    ESP_LOGI(TAG, "DBG: STREAMON OK");

    // Initialize quirc
    ESP_LOGI(TAG, "DBG: quirc_new...");
    s_qr = quirc_new();
    if (!s_qr) {
        ESP_LOGE(TAG, "quirc_new failed");
        close(s_cam_fd);
        s_cam_fd = -1;
        return false;
    }
    ESP_LOGI(TAG, "DBG: quirc_resize %dx%d...", QR_W, QR_H);
    if (quirc_resize(s_qr, QR_W, QR_H) < 0) {
        ESP_LOGE(TAG, "quirc_resize failed");
        quirc_destroy(s_qr);
        s_qr = nullptr;
        close(s_cam_fd);
        s_cam_fd = -1;
        return false;
    }

    // Grayscale buffer
    ESP_LOGI(TAG, "DBG: gray buf...");
    s_gray_buf = (uint8_t *)heap_caps_malloc(QR_W * QR_H, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "DBG: gray buf -> %p", s_gray_buf);

    // PPA client for camera preview
    ESP_LOGI(TAG, "DBG: PPA register...");
    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_SRM;
    ppa_cfg.max_pending_trans_num = 1;
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_cam_ppa));

    // Manual ISP tuning: white balance + brightness
    // Note: PPA rgb_swap inverts R↔B, so ISP red_gain → screen blue, ISP blue_gain → screen red
    int isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (isp_fd >= 0) {
        // White balance: fix green tint from Bayer BGGR demosaic
        esp_video_isp_wb_t wb = {};
        wb.enable = true;
        wb.red_gain = 1.2f;   // screen blue boost
        wb.blue_gain = 1.6f;  // screen red boost

        struct v4l2_ext_control control = {};
        control.id = V4L2_CID_USER_ESP_ISP_WB;
        control.p_u8 = (uint8_t *)&wb;
        control.size = sizeof(wb);

        struct v4l2_ext_controls controls = {};
        controls.ctrl_class = V4L2_CTRL_CLASS_USER;
        controls.count = 1;
        controls.controls = &control;

        if (ioctl(isp_fd, VIDIOC_S_EXT_CTRLS, &controls) == 0) {
            ESP_LOGI(TAG, "ISP WB set: red=%.1f blue=%.1f", wb.red_gain, wb.blue_gain);
        } else {
            ESP_LOGW(TAG, "ISP WB set failed");
        }

        // Brightness: compensate for no auto-exposure (range: -128 to 127)
        struct v4l2_control brightness = {};
        brightness.id = V4L2_CID_BRIGHTNESS;
        brightness.value = 50;

        if (ioctl(isp_fd, VIDIOC_S_CTRL, &brightness) == 0) {
            ESP_LOGI(TAG, "ISP brightness set: %ld", (long)brightness.value);
        } else {
            ESP_LOGW(TAG, "ISP brightness set failed");
        }

        close(isp_fd);
    } else {
        ESP_LOGW(TAG, "Could not open ISP device");
    }

    ESP_LOGI(TAG, "Camera + QR scanner initialized");
    return true;
}

// ============================================================
// Camera preview → DSI via PPA (rotate 270° for portrait)
// ============================================================

static void camera_preview_to_dsi(const uint8_t *cam_frame,
                                   esp_lcd_panel_handle_t panel)
{
    if (!panel || !s_cam_ppa) return;

    // Write to the current DPI back buffer (num_fbs=2 — caller will present).
    void *fb0 = dsi_get_back_fb();
    if (!fb0) return;

    // PPA SRM: rotate camera 1280x720 RGB565 → 720x1280 DPI (portrait), full
    // screen. Bars are composited on top by the caller after this returns.
    ppa_srm_oper_config_t srm = {};

    srm.in.buffer = cam_frame;
    srm.in.pic_w = CAM_W;
    srm.in.pic_h = CAM_H;
    srm.in.block_w = CAM_W;
    srm.in.block_h = CAM_H;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.out.buffer = fb0;
    srm.out.buffer_size = DSI_LCD_W * DSI_LCD_H * 2;
    srm.out.pic_w = DSI_LCD_W;
    srm.out.pic_h = DSI_LCD_H;
    srm.out.block_offset_x = 0;
    srm.out.block_offset_y = 0;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    srm.scale_x = 1.0f;
    srm.scale_y = 1.0f;
    srm.mirror_x = false;
    srm.mirror_y = false;
    srm.rgb_swap = true;   // ST7123 panel expects R↔B swap
    srm.byte_swap = false;
    srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    srm.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_cam_ppa, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM preview failed: %s", esp_err_to_name(ret));
    }
}

// ============================================================
// Scan one frame for QR code
// ============================================================

bool camera_scan_qr(char *qr_buf, int qr_buf_size,
                    esp_lcd_panel_handle_t preview_panel)
{
    if (s_cam_fd < 0 || !s_qr) return false;

    // Dequeue a frame
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_cam_fd, VIDIOC_DQBUF, &buf) < 0) {
        return false;
    }

    const uint8_t *frame = s_cam_buffers[buf.index];

    // Show preview on DSI. The static overlay strip lives outside the PPA
    // output rectangle, so it persists across frames without per-frame work.
    if (preview_panel) {
        camera_preview_to_dsi(frame, preview_panel);
    }

    // Convert to grayscale (2x downscale) for QR detection
    rgb565_to_gray_half((const uint16_t *)frame, CAM_W, CAM_H,
                         s_gray_buf, QR_W, QR_H);

    // Copy grayscale into quirc buffer
    uint8_t *qr_image = quirc_begin(s_qr, NULL, NULL);
    memcpy(qr_image, s_gray_buf, QR_W * QR_H);
    quirc_end(s_qr);

    // Queue buffer back
    if (ioctl(s_cam_fd, VIDIOC_QBUF, &buf) < 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF failed");
    }

    // Check for QR codes
    int count = quirc_count(s_qr);
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(s_qr, i, &code);

        if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
            int len = data.payload_len;
            if (len >= qr_buf_size) len = qr_buf_size - 1;
            memcpy(qr_buf, data.payload, len);
            qr_buf[len] = '\0';
            // Log only once per unique QR (suppress repeat spam)
            static char s_last_qr[128] = {};
            if (strncmp(s_last_qr, qr_buf, sizeof(s_last_qr)) != 0) {
                strncpy(s_last_qr, qr_buf, sizeof(s_last_qr) - 1);
                ESP_LOGI(TAG, "QR detected (%d bytes, dtype=%d):", len, data.data_type);
                ESP_LOG_BUFFER_HEX(TAG, qr_buf, len > 64 ? 64 : len);
            }
            return true;
        }
    }

    return false;
}

void camera_deinit()
{
    if (s_cam_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_cam_fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < CAM_BUF_COUNT; i++) {
            if (s_cam_buffers[i]) {
                munmap(s_cam_buffers[i], s_cam_buf_sizes[i]);
                s_cam_buffers[i] = nullptr;
            }
        }
        close(s_cam_fd);
        s_cam_fd = -1;
    }
    if (s_qr) {
        quirc_destroy(s_qr);
        s_qr = nullptr;
    }
    if (s_gray_buf) {
        heap_caps_free(s_gray_buf);
        s_gray_buf = nullptr;
    }
    if (s_cam_ppa) {
        ppa_unregister_client(s_cam_ppa);
        s_cam_ppa = nullptr;
    }
    ESP_LOGI(TAG, "Camera deinitialized");
}
