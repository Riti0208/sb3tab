#include <audio.hpp>
#include <sprite.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "esp32_audio";

// I2S pins for Tab5 ES8388 codec
#define I2S_MCLK_PIN    GPIO_NUM_30
#define I2S_BCLK_PIN    GPIO_NUM_27
#define I2S_LRCLK_PIN   GPIO_NUM_29
#define I2S_DOUT_PIN    GPIO_NUM_26

// Audio config
#define AUDIO_SAMPLE_RATE  48000
#define I2S_DMA_BUF_COUNT  4
#define I2S_DMA_BUF_LEN    1024

struct PCMSound {
    int16_t *samples;       // PCM 16-bit mono in PSRAM
    size_t num_samples;
    int sample_rate;
    bool loaded;
    bool playing;
    size_t play_pos;
    float volume;           // 0.0 - 1.0
    float pitch;            // Scratch pitch value (0=normal, +10=octave up, -10=octave down)
    float pan;              // -100 (full left) .. 0 (center) .. +100 (full right)
    float frac_pos;         // fractional playback position for pitch resampling
};

static std::unordered_map<std::string, PCMSound> s_sounds;
std::unordered_map<std::string, Sound> SoundPlayer::soundsPlaying;

static i2s_chan_handle_t s_i2s_tx = nullptr;
static SemaphoreHandle_t s_audio_mutex = nullptr;
static TaskHandle_t s_mixer_task = nullptr;
static bool s_audio_init = false;

// WAV header parser
struct WavHeader {
    int sample_rate;
    int bits_per_sample;
    int num_channels;
    int data_offset;
    int data_size;
    bool valid;
};

static WavHeader parse_wav(const uint8_t *data, size_t len) {
    WavHeader h = {};
    if (len < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return h;
    }

    // Find fmt chunk
    size_t pos = 12;
    while (pos + 8 <= len) {
        uint32_t chunk_id = *(uint32_t *)(data + pos);
        uint32_t chunk_size = *(uint32_t *)(data + pos + 4);

        if (memcmp(&chunk_id, "fmt ", 4) == 0) {
            if (pos + 8 + 16 > len) return h;
            uint16_t format = *(uint16_t *)(data + pos + 8);
            h.num_channels = *(uint16_t *)(data + pos + 10);
            h.sample_rate = *(uint32_t *)(data + pos + 12);
            h.bits_per_sample = *(uint16_t *)(data + pos + 22);
            if (format != 1) { // PCM only
                ESP_LOGW(TAG, "WAV format %d not supported (PCM=1 only)", format);
                return h;
            }
        } else if (memcmp(&chunk_id, "data", 4) == 0) {
            h.data_offset = pos + 8;
            h.data_size = chunk_size;
            h.valid = true;
            return h;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++; // padding
    }
    return h;
}

// Decode WAV to mono int16 at original sample rate (no resampling)
static void decode_wav_raw(const uint8_t *data, const WavHeader &wav,
                           int16_t *out, int num_samples) {
    const uint8_t *src = data + wav.data_offset;
    int bytes_per_frame = (wav.bits_per_sample / 8) * wav.num_channels;

    for (int i = 0; i < num_samples; i++) {
        int offset = i * bytes_per_frame;
        int32_t sample = 0;

        if (wav.bits_per_sample == 16) {
            for (int ch = 0; ch < wav.num_channels; ch++) {
                sample += *(int16_t *)(src + offset + ch * 2);
            }
            sample /= wav.num_channels;
        } else if (wav.bits_per_sample == 8) {
            for (int ch = 0; ch < wav.num_channels; ch++) {
                sample += ((int)src[offset + ch] - 128) * 256;
            }
            sample /= wav.num_channels;
        }

        out[i] = (int16_t)std::clamp(sample, (int32_t)-32768, (int32_t)32767);
    }
}

// Convert WAV data to 16-bit mono PCM resampled to AUDIO_SAMPLE_RATE with linear interpolation
static PCMSound decode_wav(const uint8_t *data, size_t len) {
    PCMSound pcm = {};
    WavHeader wav = parse_wav(data, len);
    if (!wav.valid) {
        ESP_LOGE(TAG, "Invalid WAV");
        return pcm;
    }

    ESP_LOGI(TAG, "WAV: %dHz %dbit %dch, data=%d bytes",
             wav.sample_rate, wav.bits_per_sample, wav.num_channels, wav.data_size);

    int src_samples = wav.data_size / (wav.bits_per_sample / 8) / wav.num_channels;

    // First decode to raw mono at original rate
    int16_t *raw = (int16_t *)heap_caps_malloc(src_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!raw) {
        ESP_LOGE(TAG, "Failed to alloc raw %d samples", src_samples);
        return pcm;
    }
    decode_wav_raw(data, wav, raw, src_samples);

    // Resample to AUDIO_SAMPLE_RATE with linear interpolation
    if (wav.sample_rate == AUDIO_SAMPLE_RATE) {
        // No resampling needed
        pcm.samples = raw;
        pcm.num_samples = src_samples;
    } else {
        double ratio = (double)AUDIO_SAMPLE_RATE / wav.sample_rate;
        int dst_samples = (int)(src_samples * ratio);

        pcm.samples = (int16_t *)heap_caps_malloc(dst_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!pcm.samples) {
            ESP_LOGE(TAG, "Failed to alloc resampled %d samples", dst_samples);
            heap_caps_free(raw);
            return pcm;
        }

        for (int i = 0; i < dst_samples; i++) {
            double src_pos = i / ratio;
            int idx0 = (int)src_pos;
            int idx1 = idx0 + 1;
            if (idx0 >= src_samples) idx0 = src_samples - 1;
            if (idx1 >= src_samples) idx1 = src_samples - 1;
            float frac = (float)(src_pos - idx0);

            pcm.samples[i] = (int16_t)(raw[idx0] * (1.0f - frac) + raw[idx1] * frac);
        }

        pcm.num_samples = dst_samples;
        heap_caps_free(raw);
    }

    pcm.sample_rate = AUDIO_SAMPLE_RATE;
    pcm.loaded = true;
    pcm.volume = 1.0f;

    return pcm;
}

// HTTP download for sounds from Scratch CDN
struct AudioDLBuf {
    uint8_t *data;
    size_t len, cap;
};

static esp_err_t audio_dl_handler(esp_http_client_event_t *evt) {
    AudioDLBuf *b = (AudioDLBuf *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (b->len + evt->data_len > b->cap) {
        size_t nc = b->cap * 2;
        while (nc < b->len + evt->data_len) nc *= 2;
        uint8_t *nd = (uint8_t *)heap_caps_realloc(b->data, nc, MALLOC_CAP_SPIRAM);
        if (!nd) return ESP_FAIL;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

static AudioDLBuf download_sound(const char *sound_id) {
    char url[256];
    snprintf(url, sizeof(url),
             "https://assets.scratch.mit.edu/internalapi/asset/%s/get/", sound_id);

    AudioDLBuf buf = {};
    buf.cap = 32 * 1024;
    buf.data = (uint8_t *)heap_caps_malloc(buf.cap, MALLOC_CAP_SPIRAM);
    if (!buf.data) return buf;

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = audio_dl_handler;
    cfg.user_data = &buf;
    cfg.buffer_size = 4096;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || buf.len == 0) {
        ESP_LOGE(TAG, "Sound download failed: %s (status=%d)", sound_id, status);
        heap_caps_free(buf.data);
        buf.data = nullptr;
        buf.len = 0;
    } else {
        ESP_LOGI(TAG, "Downloaded sound %s: %zu bytes", sound_id, buf.len);
    }
    return buf;
}

// Mixer task: mixes all playing sounds and writes to I2S
static void mixer_task(void *param) {
    const int buf_samples = I2S_DMA_BUF_LEN;
    // Stereo output buffer (L+R interleaved)
    int16_t *out_buf = (int16_t *)heap_caps_malloc(buf_samples * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!out_buf) {
        ESP_LOGE(TAG, "Failed to alloc mixer buffer");
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        memset(out_buf, 0, buf_samples * 2 * sizeof(int16_t));
        bool any_playing = false;

        if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (auto &[id, snd] : s_sounds) {
                if (!snd.playing || !snd.loaded || !snd.samples) continue;
                any_playing = true;

                // All sounds are pre-resampled to AUDIO_SAMPLE_RATE in decode_wav
                // Apply pitch: rate = 2^(pitch/10), default 1.0
                float rate = (snd.pitch == 0.0f) ? 1.0f : powf(2.0f, snd.pitch / 10.0f);

                // Compute per-channel gain from pan [-100..100]
                // pan=-100: left=1, right=0; pan=0: left=1, right=1; pan=100: left=0, right=1
                float left_gain, right_gain;
                if (snd.pan <= 0.0f) {
                    left_gain  = 1.0f;
                    right_gain = 1.0f + snd.pan / 100.0f;  // pan=-100 -> 0.0
                } else {
                    left_gain  = 1.0f - snd.pan / 100.0f;  // pan=+100 -> 0.0
                    right_gain = 1.0f;
                }

                for (int i = 0; i < buf_samples; i++) {
                    if (snd.frac_pos >= (float)snd.num_samples) {
                        snd.playing = false;
                        snd.frac_pos = 0.0f;
                        break;
                    }

                    // Linear interpolation between adjacent samples
                    size_t idx0 = (size_t)snd.frac_pos;
                    size_t idx1 = idx0 + 1;
                    if (idx1 >= snd.num_samples) idx1 = snd.num_samples - 1;
                    float frac = snd.frac_pos - (float)idx0;
                    float interpolated = snd.samples[idx0] * (1.0f - frac) + snd.samples[idx1] * frac;

                    int32_t base = (int32_t)(interpolated * snd.volume);

                    // Apply pan gains and mix into stereo output
                    int32_t left  = out_buf[i * 2]     + (int32_t)(base * left_gain);
                    int32_t right = out_buf[i * 2 + 1] + (int32_t)(base * right_gain);
                    out_buf[i * 2]     = (int16_t)std::clamp(left,  (int32_t)-32768, (int32_t)32767);
                    out_buf[i * 2 + 1] = (int16_t)std::clamp(right, (int32_t)-32768, (int32_t)32767);

                    snd.frac_pos += rate;
                }
                // Keep integer play_pos in sync for getMusicPosition()
                snd.play_pos = (size_t)snd.frac_pos;
            }
            xSemaphoreGive(s_audio_mutex);
        }

        size_t bytes_written = 0;
        i2s_channel_write(s_i2s_tx, out_buf, buf_samples * 2 * sizeof(int16_t),
                          &bytes_written, portMAX_DELAY);

        if (!any_playing) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ============================================================
// SoundPlayer interface implementation
// ============================================================

bool SoundPlayer::init() {
    if (s_audio_init) return true;

    s_audio_mutex = xSemaphoreCreateMutex();

    // Configure I2S for Tab5 ES8388
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCLK_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return false;
    }

    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return false;
    }

    s_audio_init = true;
    ESP_LOGI(TAG, "I2S audio initialized (BCLK=%d, LRCLK=%d, DOUT=%d, %dHz)",
             I2S_BCLK_PIN, I2S_LRCLK_PIN, I2S_DOUT_PIN, AUDIO_SAMPLE_RATE);

    // Start mixer task
    xTaskCreatePinnedToCore(mixer_task, "audio_mix", 8192, nullptr, 6, &s_mixer_task, 1);

    return true;
}

void SoundPlayer::startSoundLoaderThread(Sprite *sprite, mz_zip_archive *zip,
                                          const std::string &soundId,
                                          const bool &streamed,
                                          const bool &fromProject,
                                          const bool &fromCache) {
    if (!init()) return;

    // Already loaded? Just play it
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end() && it->second.loaded) {
            it->second.play_pos = 0;
            it->second.frac_pos = 0.0f;
            it->second.playing = true;
            if (sprite) it->second.volume = sprite->volume / 100.0f;
            xSemaphoreGive(s_audio_mutex);
            ESP_LOGI(TAG, "Playing cached sound: %s", soundId.c_str());
            return;
        }
        xSemaphoreGive(s_audio_mutex);
    }

    // Download from CDN
    ESP_LOGI(TAG, "Downloading sound: %s", soundId.c_str());
    AudioDLBuf raw = download_sound(soundId.c_str());
    if (!raw.data || raw.len == 0) {
        ESP_LOGE(TAG, "Failed to download sound: %s", soundId.c_str());
        return;
    }

    // Decode WAV to PCM
    PCMSound pcm = decode_wav(raw.data, raw.len);
    heap_caps_free(raw.data);

    if (!pcm.loaded) {
        ESP_LOGE(TAG, "Failed to decode sound: %s", soundId.c_str());
        return;
    }

    ESP_LOGI(TAG, "Decoded sound: %s (%zu samples, %dHz)",
             soundId.c_str(), pcm.num_samples, pcm.sample_rate);

    // Start playing immediately
    pcm.play_pos = 0;
    pcm.frac_pos = 0.0f;
    pcm.playing = true;
    if (sprite) pcm.volume = sprite->volume / 100.0f;

    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_sounds[soundId] = pcm;
        xSemaphoreGive(s_audio_mutex);
    }
}

bool SoundPlayer::loadSoundFromMemory(const std::string &soundId, const uint8_t *data, size_t len) {
    if (!init()) return false;

    PCMSound pcm = decode_wav(data, len);
    if (!pcm.loaded) {
        ESP_LOGE(TAG, "Failed to decode sound from memory: %s", soundId.c_str());
        return false;
    }

    ESP_LOGI(TAG, "Loaded sound from memory: %s (%zu samples)", soundId.c_str(), pcm.num_samples);

    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_sounds[soundId] = pcm;
        xSemaphoreGive(s_audio_mutex);
    }
    return true;
}

bool SoundPlayer::loadSoundFromSB3(Sprite *sprite, mz_zip_archive *zip,
                                    const std::string &soundId, const bool &streamed) {
    startSoundLoaderThread(sprite, zip, soundId, streamed, true, false);
    return true;
}

bool SoundPlayer::loadSoundFromFile(Sprite *sprite, std::string fileName,
                                     const bool &streamed, const bool &fromCache) {
    startSoundLoaderThread(sprite, nullptr, fileName, streamed, false, fromCache);
    return true;
}

int SoundPlayer::playSound(const std::string &soundId) {
    if (!s_audio_mutex) return -1;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end() && it->second.loaded) {
            it->second.play_pos = 0;
            it->second.frac_pos = 0.0f;
            it->second.playing = true;
            xSemaphoreGive(s_audio_mutex);
            return 0;
        }
        xSemaphoreGive(s_audio_mutex);
    }
    return -1;
}

void SoundPlayer::setSoundVolume(const std::string &soundId, float volume) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            it->second.volume = std::clamp(volume / 100.0f, 0.0f, 1.0f);
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

float SoundPlayer::getSoundVolume(const std::string &soundId) {
    if (!s_audio_mutex) return 0.0f;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            float v = it->second.volume * 100.0f;
            xSemaphoreGive(s_audio_mutex);
            return v;
        }
        xSemaphoreGive(s_audio_mutex);
    }
    return 0.0f;
}

void SoundPlayer::setPitch(const std::string &soundId, float pitch) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            it->second.pitch = pitch;
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::setPan(const std::string &soundId, float pan) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            it->second.pan = std::clamp(pan, -100.0f, 100.0f);
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

double SoundPlayer::getMusicPosition(const std::string &soundId) {
    if (!s_audio_mutex) return 0.0;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end() && it->second.sample_rate > 0) {
            double pos = (double)it->second.play_pos / it->second.sample_rate;
            xSemaphoreGive(s_audio_mutex);
            return pos;
        }
        xSemaphoreGive(s_audio_mutex);
    }
    return 0.0;
}

void SoundPlayer::setMusicPosition(double position, const std::string &soundId) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end() && it->second.sample_rate > 0) {
            it->second.play_pos = (size_t)(position * it->second.sample_rate);
            if (it->second.play_pos >= it->second.num_samples)
                it->second.play_pos = 0;
            it->second.frac_pos = (float)it->second.play_pos;
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::stopSound(const std::string &soundId) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            it->second.playing = false;
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::stopStreamedSound() {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (auto &[id, snd] : s_sounds) {
            snd.playing = false;
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::checkAudio() {
    // Handled by mixer task
}

bool SoundPlayer::isSoundPlaying(const std::string &soundId) {
    if (!s_audio_mutex) return false;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            bool p = it->second.playing;
            xSemaphoreGive(s_audio_mutex);
            return p;
        }
        xSemaphoreGive(s_audio_mutex);
    }
    return false;
}

bool SoundPlayer::isSoundLoaded(const std::string &soundId) {
    if (!s_audio_mutex) return false;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            bool l = it->second.loaded;
            xSemaphoreGive(s_audio_mutex);
            return l;
        }
        xSemaphoreGive(s_audio_mutex);
    }
    return false;
}

void SoundPlayer::freeAudio(const std::string &soundId) {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            if (it->second.samples) {
                heap_caps_free(it->second.samples);
            }
            s_sounds.erase(it);
        }
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::flushAudio() {
    // Free sounds that haven't been played recently
}

void SoundPlayer::cleanupAudio() {
    if (!s_audio_mutex) return;
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        for (auto &[id, snd] : s_sounds) {
            snd.playing = false;
            if (snd.samples) {
                heap_caps_free(snd.samples);
                snd.samples = nullptr;
            }
        }
        s_sounds.clear();
        xSemaphoreGive(s_audio_mutex);
    }
}

void SoundPlayer::deinit() {
    cleanupAudio();
    if (s_mixer_task) {
        vTaskDelete(s_mixer_task);
        s_mixer_task = nullptr;
    }
    if (s_i2s_tx) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = nullptr;
    }
    s_audio_init = false;
}
