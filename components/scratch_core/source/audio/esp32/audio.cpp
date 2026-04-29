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
    int16_t *samples;       // PCM 16-bit mono — owned heap buffer OR borrowed
                            // pointer into the caller's source buffer (see
                            // samples_owned). Never written to by the mixer.
    bool samples_owned;     // true → heap_caps_free(samples) on cleanup;
                            // false → samples points into g_assets, do NOT free
    size_t num_samples;
    int sample_rate;
    bool loaded;
    bool failed;            // permanent failure marker — don't retry download/decode
    bool playing;
    size_t play_pos;
    float volume;           // 0.0 - 1.0
    float pitch;            // Scratch pitch value (0=normal, +10=octave up, -10=octave down)
    float pan;              // -100 (full left) .. 0 (center) .. +100 (full right)
    double frac_pos;        // fractional playback position for SRC + pitch.
                            // Must be double: at ~2M source samples a float's
                            // 24-bit mantissa quantises the fractional part to
                            // ~1/8 sample, causing audible pitch wobble on long WAVs.
};

static std::unordered_map<std::string, PCMSound> s_sounds;
std::unordered_map<std::string, Sound> SoundPlayer::soundsPlaying;

static i2s_chan_handle_t s_i2s_tx = nullptr;
static SemaphoreHandle_t s_audio_mutex = nullptr;
static TaskHandle_t s_mixer_task = nullptr;
static bool s_audio_init = false;
static volatile bool s_audio_suspended = false;  // pause flag for in-game overlays

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

// Decode WAV to 16-bit mono PCM at the WAV's original sample rate.
// Sample-rate conversion is folded into the mixer's existing linear-interpolation
// loop (rate = src_rate / 48000 × pitch).
//
// Fast path (16-bit mono): point pcm.samples directly at the source buffer's
// data section — no allocation, no copy. Critical for the 4 MB Ninja BGM where
// duplicating into PSRAM doubles peak usage and fails on the 2nd game load due
// to fragmentation. Caller MUST keep `data` alive for the sound's lifetime.
// In our flow that's g_assets, which is freed AFTER cleanupAudio.
//
// Slow path (8-bit, multi-channel, etc.): allocate + decode to mono int16.
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
    if (src_samples <= 0) {
        ESP_LOGE(TAG, "WAV has no samples");
        return pcm;
    }

    if (wav.bits_per_sample == 16 && wav.num_channels == 1) {
        // Borrow source buffer — no PSRAM cost.
        pcm.samples = (int16_t *)(data + wav.data_offset);
        pcm.samples_owned = false;
    } else {
        pcm.samples = (int16_t *)heap_caps_malloc(src_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!pcm.samples) {
            ESP_LOGE(TAG, "Failed to alloc %d samples (%d bytes) — PSRAM free=%u largest=%u",
                     src_samples, src_samples * 2,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return pcm;
        }
        decode_wav_raw(data, wav, pcm.samples, src_samples);
        pcm.samples_owned = true;
    }

    pcm.num_samples = src_samples;
    pcm.sample_rate = wav.sample_rate;
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
        bool suspended = s_audio_suspended;

        if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (auto &[id, snd] : s_sounds) {
                if (!snd.playing || !snd.loaded || !snd.samples) continue;
                // While suspended, freeze project sounds (don't advance frac_pos
                // so resume picks up from the same instant) but let "ui_*" UI
                // effect sounds play through — overlays still need feedback.
                if (suspended && id.compare(0, 3, "ui_") != 0) continue;
                any_playing = true;

                // Sample-rate conversion is folded into this loop: advance the source
                // index by (src_rate / output_rate) per output sample, then multiply by
                // the Scratch pitch factor. linear interpolation below handles fractional
                // positions for both SRC and pitch shift in one pass.
                double rate = (double)snd.sample_rate / (double)AUDIO_SAMPLE_RATE;
                if (snd.pitch != 0.0f) rate *= pow(2.0, snd.pitch / 10.0);

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
                    if (snd.frac_pos >= (double)snd.num_samples) {
                        snd.playing = false;
                        snd.frac_pos = 0.0;
                        break;
                    }

                    // Linear interpolation between adjacent samples
                    size_t idx0 = (size_t)snd.frac_pos;
                    size_t idx1 = idx0 + 1;
                    if (idx1 >= snd.num_samples) idx1 = snd.num_samples - 1;
                    float frac = (float)(snd.frac_pos - (double)idx0);
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

    // Already loaded or previously failed? Skip download
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end()) {
            if (it->second.failed) {
                xSemaphoreGive(s_audio_mutex);
                return;  // permanent failure, don't retry
            }
            if (it->second.loaded) {
                it->second.play_pos = 0;
                it->second.frac_pos = 0.0f;
                it->second.playing = true;
                if (sprite) it->second.volume = sprite->volume / 100.0f;
                xSemaphoreGive(s_audio_mutex);
                ESP_LOGI(TAG, "Playing cached sound: %s", soundId.c_str());
                return;
            }
        }
        xSemaphoreGive(s_audio_mutex);
    }

    // Download from CDN
    ESP_LOGI(TAG, "Downloading sound: %s", soundId.c_str());
    AudioDLBuf raw = download_sound(soundId.c_str());
    if (!raw.data || raw.len == 0) {
        ESP_LOGE(TAG, "Failed to download sound: %s — marking as failed", soundId.c_str());
        if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
            PCMSound failed_pcm = {};
            failed_pcm.failed = true;
            s_sounds[soundId] = failed_pcm;
            xSemaphoreGive(s_audio_mutex);
        }
        return;
    }

    // Decode WAV to PCM
    PCMSound pcm = decode_wav(raw.data, raw.len);
    heap_caps_free(raw.data);

    if (!pcm.loaded) {
        ESP_LOGE(TAG, "Failed to decode sound: %s — marking as failed", soundId.c_str());
        if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
            PCMSound failed_pcm = {};
            failed_pcm.failed = true;
            s_sounds[soundId] = failed_pcm;
            xSemaphoreGive(s_audio_mutex);
        }
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

    // Dedup: many sprites in a project share the same sound id (the Ninja demo
    // calls us 8× for the same 83a9787... clip). Skip re-decoding when we
    // already have a result — loaded or permanently failed.
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto it = s_sounds.find(soundId);
        if (it != s_sounds.end() && (it->second.loaded || it->second.failed)) {
            bool ok = it->second.loaded;
            xSemaphoreGive(s_audio_mutex);
            return ok;
        }
        xSemaphoreGive(s_audio_mutex);
    }

    ESP_LOGI(TAG, "loadSoundFromMemory: %s len=%zu psram_free=%u largest=%u",
             soundId.c_str(), len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    PCMSound pcm = decode_wav(data, len);
    if (!pcm.loaded) {
        ESP_LOGE(TAG, "Failed to decode sound from memory: %s — marking as failed", soundId.c_str());
        if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
            PCMSound failed_pcm = {};
            failed_pcm.failed = true;
            s_sounds[soundId] = failed_pcm;
            xSemaphoreGive(s_audio_mutex);
        }
        return false;
    }

    ESP_LOGI(TAG, "Loaded sound from memory: %s (%zu samples)", soundId.c_str(), pcm.num_samples);

    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_sounds[soundId] = pcm;
        xSemaphoreGive(s_audio_mutex);
    } else {
        // Mutex contention — leak rather than store half-state. Should be rare.
        ESP_LOGE(TAG, "loadSoundFromMemory: mutex timeout for %s, leaking %zu samples",
                 soundId.c_str(), pcm.num_samples);
        heap_caps_free(pcm.samples);
        return false;
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
            it->second.frac_pos = (double)it->second.play_pos;
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
            if (it->second.samples && it->second.samples_owned) {
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
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        size_t freed_count = 0, freed_bytes = 0, borrowed = 0;
        // Preserve "ui_*" sounds across game exits — those belong to the menu UI,
        // not the Scratch project, and re-loading them every game would be wasteful.
        for (auto it = s_sounds.begin(); it != s_sounds.end(); ) {
            if (it->first.compare(0, 3, "ui_") == 0) {
                it->second.playing = false;
                ++it;
                continue;
            }
            it->second.playing = false;
            if (it->second.samples) {
                if (it->second.samples_owned) {
                    freed_bytes += it->second.num_samples * sizeof(int16_t);
                    heap_caps_free(it->second.samples);
                    freed_count++;
                } else {
                    borrowed++;
                }
                it->second.samples = nullptr;
            }
            it = s_sounds.erase(it);
        }
        xSemaphoreGive(s_audio_mutex);
        ESP_LOGI(TAG, "cleanupAudio: freed %zu owned (%zu bytes), %zu borrowed dropped, psram_free=%u largest=%u",
                 freed_count, freed_bytes, borrowed,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "cleanupAudio: mutex timeout — sounds NOT freed");
    }
}

// Exposed to main.cpp via es8388_audio.h. Kept off the SoundPlayer class so the
// shared audio.hpp doesn't grow esp32-specific plumbing.
void audio_mixer_set_suspended(bool suspended) {
    s_audio_suspended = suspended;
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
