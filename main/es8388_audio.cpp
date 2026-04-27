#include "es8388_audio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <audio.hpp>

static const char *TAG = "es8388";

// ES8388 7-bit I2C address on Tab5 (CE pin LOW → 7-bit addr 0x10)
// Note: BSP uses 8-bit addr 0x20, but i2c_master driver takes 7-bit
#define ES8388_ADDR  0x10

// ES8388 register addresses
#define ES_CONTROL1      0x00
#define ES_CONTROL2      0x01
#define ES_CHIPPOWER     0x02
#define ES_ADCPOWER      0x03
#define ES_DACPOWER      0x04
#define ES_MASTERMODE    0x08
#define ES_DACCONTROL1   0x17
#define ES_DACCONTROL2   0x18
#define ES_DACCONTROL3   0x19
#define ES_DACCONTROL4   0x1A
#define ES_DACCONTROL5   0x1B
#define ES_DACCONTROL16  0x26
#define ES_DACCONTROL17  0x27
#define ES_DACCONTROL20  0x2A
#define ES_DACCONTROL21  0x2B
#define ES_DACCONTROL23  0x2D
#define ES_DACCONTROL24  0x2E
#define ES_DACCONTROL25  0x2F
#define ES_DACCONTROL26  0x30
#define ES_DACCONTROL27  0x31

extern i2c_master_bus_handle_t g_i2c_bus;
static i2c_master_dev_handle_t s_es8388_dev = NULL;

static esp_err_t es_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(s_es8388_dev, buf, 2, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write reg 0x%02X=0x%02X FAILED: %s", reg, val, esp_err_to_name(err));
    }
    return err;
}

bool es8388_audio_init()
{
    if (!g_i2c_bus) {
        ESP_LOGE(TAG, "I2C bus not initialized");
        return false;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = ES8388_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    esp_err_t err = i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &s_es8388_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ES8388 device: %s", esp_err_to_name(err));
        return false;
    }

    // --- Init sequence matching Tab5 BSP (esp_codec_dev ES8388 driver) ---

    // Mute DAC during init
    es_write_reg(ES_DACCONTROL3, 0x04);
    // Normal operation
    es_write_reg(ES_CONTROL2, 0x50);
    // Power up all
    es_write_reg(ES_CHIPPOWER, 0x00);
    // Slave mode
    es_write_reg(ES_MASTERMODE, 0x00);
    // DAC power off initially
    es_write_reg(ES_DACPOWER, 0xC0);
    // Play & Record mode
    es_write_reg(ES_CONTROL1, 0x12);

    // DAC I2S format: 16-bit, Philips I2S
    es_write_reg(ES_DACCONTROL1, 0x18);
    // Single speed, MCLK/LRCK = 256
    es_write_reg(ES_DACCONTROL2, 0x02);

    // Mixer: LIN1/RIN1, no analog bypass
    es_write_reg(ES_DACCONTROL16, 0x00);
    // Left DAC to left mixer only, 0dB
    es_write_reg(ES_DACCONTROL17, 0x90);
    // Right DAC to right mixer only, 0dB
    es_write_reg(ES_DACCONTROL20, 0x90);
    // ADC/DAC share LRCK, DAC enabled
    es_write_reg(ES_DACCONTROL21, 0x80);
    // vroi = 0
    es_write_reg(ES_DACCONTROL23, 0x00);

    // Output volume: LOUT1/ROUT1 = 0dB
    es_write_reg(ES_DACCONTROL24, 0x1E);
    es_write_reg(ES_DACCONTROL25, 0x1E);
    // LOUT2/ROUT2 off
    es_write_reg(ES_DACCONTROL26, 0x00);
    es_write_reg(ES_DACCONTROL27, 0x00);

    // DAC digital volume: ~80% (BSP default vol=80 → ~0x06)
    es_write_reg(ES_DACCONTROL4, 0x06);
    es_write_reg(ES_DACCONTROL5, 0x06);

    // Enable DAC + all outputs
    es_write_reg(ES_DACPOWER, 0x3C);
    // Power down ADC (DAC-only mode)
    es_write_reg(ES_ADCPOWER, 0xFF);

    // Unmute DAC
    es_write_reg(ES_DACCONTROL3, 0x00);

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ES8388 codec initialized (addr=0x%02X, DAC mode, I2S slave, 16-bit)", ES8388_ADDR);
    return true;
}

void es8388_test_tone_22k()
{
    // Generate 22050Hz sine, resample to 48kHz, write DIRECTLY to I2S (bypass mixer)
    // This tests: is distortion from mixer task, or from I2S/ES8388?
    ESP_LOGI(TAG, "Test: 22050Hz sine resampled to 48kHz, DIRECT I2S write (no mixer)");

    const int src_rate = 22050;
    const int dst_rate = 48000;
    const int duration = 2;
    const int src_samples = src_rate * duration;
    const double ratio = (double)dst_rate / src_rate;
    const int dst_samples = (int)(src_samples * ratio);

    // Generate source sine at 22050Hz
    int16_t *src = (int16_t *)heap_caps_malloc(src_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!src) { ESP_LOGE(TAG, "src malloc failed"); return; }
    for (int i = 0; i < src_samples; i++)
        src[i] = (int16_t)(8000.0f * sinf(2.0f * 3.14159265f * 440.0f * i / src_rate));

    // Resample to 48kHz with linear interpolation
    int16_t *dst = (int16_t *)heap_caps_malloc(dst_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!dst) { heap_caps_free(src); ESP_LOGE(TAG, "dst malloc failed"); return; }
    for (int i = 0; i < dst_samples; i++) {
        double pos = i / ratio;
        int idx0 = (int)pos;
        int idx1 = idx0 + 1;
        if (idx1 >= src_samples) idx1 = src_samples - 1;
        float frac = (float)(pos - idx0);
        dst[i] = (int16_t)(src[idx0] * (1.0f - frac) + src[idx1] * frac);
    }
    heap_caps_free(src);

    // Direct I2S write (same as es8388_test_tone but with resampled data)
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 1024;

    if (i2s_new_channel(&chan_cfg, &tx, NULL) != ESP_OK) {
        heap_caps_free(dst); return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(dst_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_30, .bclk = GPIO_NUM_27, .ws = GPIO_NUM_29,
            .dout = GPIO_NUM_26, .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) {
        i2s_del_channel(tx); heap_caps_free(dst); return;
    }

    // Write resampled mono data as stereo to I2S
    const int chunk = 1024;
    int16_t *stereo = (int16_t *)heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int pos = 0;
    while (pos < dst_samples) {
        int n = (dst_samples - pos < chunk) ? (dst_samples - pos) : chunk;
        for (int i = 0; i < n; i++) {
            stereo[i * 2] = dst[pos + i];
            stereo[i * 2 + 1] = dst[pos + i];
        }
        size_t bw = 0;
        i2s_channel_write(tx, stereo, n * 2 * sizeof(int16_t), &bw, pdMS_TO_TICKS(1000));
        pos += n;
    }

    // Silence flush
    memset(stereo, 0, chunk * 2 * sizeof(int16_t));
    size_t bw = 0;
    i2s_channel_write(tx, stereo, chunk * 2 * sizeof(int16_t), &bw, pdMS_TO_TICKS(100));

    heap_caps_free(stereo);
    heap_caps_free(dst);
    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    ESP_LOGI(TAG, "Direct I2S test done (wrote %d resampled samples)", dst_samples);
}

void es8388_set_mute(bool mute)
{
    if (!s_es8388_dev) return;
    // DACCONTROL3 bit[5]: DAC soft mute
    es_write_reg(ES_DACCONTROL3, mute ? 0x24 : 0x00);
    ESP_LOGI(TAG, "Mute: %s", mute ? "ON" : "OFF");
}
