#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/gpio.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include "hal/usb_serial_jtag_ll.h"

// ScratchEverywhere headers
#include <runtime.hpp>
#include <parser.hpp>
#include <unzip.hpp>
#include <render.hpp>
#include <blockExecutor.hpp>
#include <nlohmann/json.hpp>

#include <audio.hpp>
#include <renderers/headless/speech_manager_headless.hpp>
#include <renderers/headless/headless_fb.hpp>
#include "sw_renderer.h"
#include "lcd_driver.h"
#include "dsi_display.h"

// Build for DSI (Tab5) or SPI LCD
#define USE_DSI_DISPLAY 1

static const char *TAG = "scratcher";

extern BlockResult nopBlock(Block &, Sprite *, bool *, bool);
__attribute__((used)) static auto *_force_blocks = &nopBlock;

static volatile bool scratch_running = false;
static SemaphoreHandle_t sprite_mutex = nullptr;
static SWRenderer *renderer = nullptr;
#if USE_DSI_DISPLAY
static esp_lcd_panel_handle_t dsi_panel = nullptr;
#else
static esp_lcd_panel_handle_t lcd_panel = nullptr;
static uint16_t *lcd_fb = nullptr;
#endif

// ============================================================
// USB Serial/JTAG LL API
// ============================================================

static void usb_ll_write(const char *str) {
    const uint8_t *p = (const uint8_t *)str;
    size_t remaining = strlen(str);
    while (remaining > 0) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            int written = usb_serial_jtag_ll_write_txfifo(p, remaining);
            usb_serial_jtag_ll_txfifo_flush();
            p += written;
            remaining -= written;
        }
        vTaskDelay(1);
    }
}

static int usb_ll_read_bulk(uint8_t *buf, size_t size, int timeout_ms) {
    size_t received = 0;
    TickType_t last_data = xTaskGetTickCount();
    while (received < size) {
        if (usb_serial_jtag_ll_rxfifo_data_available()) {
            size_t want = size - received;
            if (want > 64) want = 64;
            int len = usb_serial_jtag_ll_read_rxfifo(buf + received, want);
            if (len > 0) {
                received += len;
                last_data = xTaskGetTickCount();
            }
        }
        vTaskDelay(1);
        if ((xTaskGetTickCount() - last_data) > pdMS_TO_TICKS(timeout_ms)) break;
    }
    return received;
}

static int usb_ll_read_byte(int timeout_ms) {
    uint8_t c;
    return (usb_ll_read_bulk(&c, 1, timeout_ms) == 1) ? c : -1;
}

static bool usb_ll_read_line(char *buf, int max_len, int timeout_ms) {
    int pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int c = usb_ll_read_byte(100);
        if (c < 0) continue;
        if (c == '\n' || c == '\r') {
            if (pos > 0) { buf[pos] = '\0'; return true; }
            continue;
        }
        if (pos < max_len - 1) buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return pos > 0;
}

// ============================================================
// Protocol: receive pre-extracted project data from Python
// Commands: JSON:<size>, ASSET:<name>:<size>, START
// ============================================================

static nlohmann::json *g_project_json = nullptr;

struct AssetEntry {
    std::string name;
    uint8_t *data;
    size_t len;
};
static std::vector<AssetEntry> g_assets;

static void free_assets() {
    for (auto &a : g_assets) {
        if (a.data) heap_caps_free(a.data);
    }
    g_assets.clear();
    if (g_project_json) { delete g_project_json; g_project_json = nullptr; }
}

static bool handle_command(const char *line) {
    if (strncmp(line, "JSON:", 5) == 0) {
        size_t size = atoi(line + 5);
        if (size == 0 || size > 4 * 1024 * 1024) {
            usb_ll_write("ERR:json size\n");
            return false;
        }
        usb_ll_write("READY\n");

        char *buf = (char *)heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
        if (!buf) { usb_ll_write("ERR:alloc\n"); return false; }

        // Timeout scales with size: ~12KB/s USB throughput + 5s margin
        int timeout_ms = (int)(size / 12) + 5000;
        int got = usb_ll_read_bulk((uint8_t *)buf, size, timeout_ms);
        if ((size_t)got != size) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ERR:got %d/%zu\n", got, size);
            usb_ll_write(msg);
            heap_caps_free(buf);
            return false;
        }
        buf[size] = '\0';

        if (g_project_json) delete g_project_json;
        g_project_json = new nlohmann::json();
        try {
            *g_project_json = nlohmann::json::parse(buf, buf + size);
        } catch (...) {
            usb_ll_write("ERR:json parse\n");
            heap_caps_free(buf);
            delete g_project_json;
            g_project_json = nullptr;
            return false;
        }
        heap_caps_free(buf);
        usb_ll_write("OK\n");
        return true;

    } else if (strncmp(line, "ASSET:", 6) == 0) {
        // ASSET:<name>:<size>
        const char *name_start = line + 6;
        const char *colon = strchr(name_start, ':');
        if (!colon) { usb_ll_write("ERR:asset fmt\n"); return false; }

        std::string name(name_start, colon - name_start);
        size_t size = atoi(colon + 1);
        if (size == 0 || size > 4 * 1024 * 1024) {
            usb_ll_write("ERR:asset size\n");
            return false;
        }
        usb_ll_write("READY\n");

        uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (!buf) { usb_ll_write("ERR:alloc\n"); return false; }

        int timeout_ms = (int)(size / 12) + 5000;
        int got = usb_ll_read_bulk(buf, size, timeout_ms);
        if ((size_t)got != size) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ERR:got %d/%zu\n", got, size);
            usb_ll_write(msg);
            heap_caps_free(buf);
            return false;
        }

        g_assets.push_back({name, buf, size});
        usb_ll_write("OK\n");
        return true;

    } else if (strcmp(line, "START") == 0) {
        return true;  // Handled by caller
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "ERR:unknown '%.60s'\n", line);
    usb_ll_write(msg);
    return false;
}

// ============================================================
// Pen callbacks for headless renderer
// ============================================================

extern "C" void render_set_pen_callbacks(
    void (*)(void),
    void (*)(void),
    void (*)(float, float, float, float, uint8_t, uint8_t, uint8_t, uint8_t, float),
    void (*)(float, float, uint8_t, uint8_t, uint8_t, uint8_t, float),
    void (*)(const char *, float, float, float, float, float)
);

static void pen_init_cb() { if (renderer) renderer->initPenLayer(); }
static void pen_clear_cb() { if (renderer) renderer->clearPenLayer(); }
static void pen_line_cb(float x1, float y1, float x2, float y2,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) {
    if (renderer) renderer->penLine(x1, y1, x2, y2, r, g, b, a, thickness);
}
static void pen_dot_cb(float x, float y,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a, float thickness) {
    if (renderer) renderer->penDot(x, y, r, g, b, a, thickness);
}
static void pen_stamp_cb(const char *costume, float x, float y,
                         float dir, float size, float ghost) {
    if (renderer) renderer->penStampSprite(costume, x, y, dir, size, ghost);
}

// ============================================================
// Scratch Runtime Task
// ============================================================

static int s_frame_count = 0;
static int64_t s_total_step = 0, s_total_render = 0, s_total_lcd = 0;

static void scratch_runtime_task(void *param)
{
    while (scratch_running) {
        xSemaphoreTake(sprite_mutex, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        auto [running, restart] = Scratch::stepScratchProject();
        int64_t t1 = esp_timer_get_time();

        if (renderer) {
            int64_t r0 = esp_timer_get_time();

            // 1+2. Draw backdrop directly (or clear to white if no backdrop)
            {
                bool drewBackdrop = false;
                if (Scratch::stageSprite) {
                    int cosIdx = Scratch::stageSprite->currentCostume;
                    if (cosIdx >= 0 && cosIdx < (int)Scratch::stageSprite->costumes.size()) {
                        drewBackdrop = renderer->drawBackdropFull(
                            Scratch::stageSprite->costumes[cosIdx].fullName);
                    }
                }
                if (!drewBackdrop) renderer->clear();
            }
            int64_t r1 = esp_timer_get_time();
            int64_t r2 = r1; // backdrop merged into clear

            // 3. Composite pen layer
            renderer->compositePenLayer();
            int64_t r3 = esp_timer_get_time();

            // 4. Draw sprites (in layer order)
            for (auto &sprite : Scratch::sprites) {
                if (sprite->isStage || !sprite->visible) continue;
                int cosIdx = sprite->currentCostume;
                if (cosIdx < 0 || cosIdx >= (int)sprite->costumes.size()) continue;
                const auto &cos = sprite->costumes[cosIdx];
                renderer->drawSprite(cos.fullName,
                                     sprite->xPosition, sprite->yPosition,
                                     sprite->rotation, sprite->size,
                                     sprite->visible, sprite->ghostEffect,
                                     sprite->brightnessEffect);
            }
            int64_t r4 = esp_timer_get_time();

            // 5. Draw speech bubbles
            SpeechManager *sm = Render::getSpeechManager();
            if (sm) {
                static_cast<SpeechManagerHeadless *>(sm)->renderToFramebuffer(
                    renderer->getFramebuffer(), STAGE_W, STAGE_H,
                    Scratch::projectWidth, Scratch::projectHeight);
            }
            int64_t r5 = esp_timer_get_time();

            // 6. Draw variable/list monitors
            g_headless_fb.fb = renderer->getFramebuffer();
            g_headless_fb.width = STAGE_W;
            g_headless_fb.height = STAGE_H;
            Render::renderMonitors();
            int64_t r6 = esp_timer_get_time();

            int64_t t2 = r6;

            // 7. Push framebuffer to display
#if USE_DSI_DISPLAY
            if (dsi_panel) {
                dsi_display_update(dsi_panel, renderer->getFramebuffer(), STAGE_W, STAGE_H);
            }
#else
            if (lcd_panel && lcd_fb) {
                rgb888_to_rgb565_scaled(renderer->getFramebuffer(),
                                        STAGE_W, STAGE_H,
                                        lcd_fb, LCD_W, LCD_H);
                lcd_draw_framebuffer(lcd_panel, lcd_fb);
            }
#endif

            int64_t t3 = esp_timer_get_time();

            s_total_step += (t1 - t0);
            s_total_render += (t2 - t1);
            s_total_lcd += (t3 - t2);
            s_frame_count++;

            // Detailed render breakdown accumulators
            static int64_t s_r_clear=0, s_r_bg=0, s_r_pen=0, s_r_spr=0, s_r_speech=0, s_r_mon=0;
            s_r_clear += (r1-r0); s_r_bg += (r2-r1); s_r_pen += (r3-r2);
            s_r_spr += (r4-r3); s_r_speech += (r5-r4); s_r_mon += (r6-r5);

            if (s_frame_count % 60 == 0) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                    "step:%lld clr:%lld bg:%lld pen:%lld spr:%lld say:%lld mon:%lld lcd:%lld\n",
                    s_total_step/60,
                    s_r_clear/60, s_r_bg/60, s_r_pen/60,
                    s_r_spr/60, s_r_speech/60, s_r_mon/60,
                    s_total_lcd/60);
                usb_ll_write(msg);
                s_total_step = s_total_render = s_total_lcd = 0;
                s_r_clear = s_r_bg = s_r_pen = s_r_spr = s_r_speech = s_r_mon = 0;
            }
        }
        xSemaphoreGive(sprite_mutex);

        if (!running) break;
        vTaskDelay(1);
    }
    scratch_running = false;
    vTaskDelete(nullptr);
}

// ============================================================
// Load and run project
// ============================================================

static bool load_and_run()
{
    if (!g_project_json) {
        usb_ll_write("ERR:no json\n");
        return false;
    }

    if (scratch_running) {
        scratch_running = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_task_wdt_deinit();

    Scratch::sprites.clear();
    SoundPlayer::cleanupAudio();

    // Initialize headless renderer (creates WindowHeadless)
    static bool render_initialized = false;
    if (!render_initialized) {
        Render::Init();
        render_set_pen_callbacks(pen_init_cb, pen_clear_cb, pen_line_cb, pen_dot_cb, pen_stamp_cb);
        render_initialized = true;
    }

    // Mark as UNZIPPED so runtime won't try to access zipArchive
    Scratch::projectType = ProjectType::UNZIPPED;

    usb_ll_write("LOADING\n");
    Parser::loadSprites(*g_project_json);

    {
        char msg[64];
        snprintf(msg, sizeof(msg), "SPRITES:%zu\n", Scratch::sprites.size());
        usb_ll_write(msg);
    }

    // Load costumes and sounds from received assets
    for (auto &sprite : Scratch::sprites) {
        for (auto &costume : sprite->costumes) {
            if (costume.fullName.empty()) continue;
            for (auto &asset : g_assets) {
                if (asset.name == costume.fullName) {
                    renderer->loadCostumeFromMemory(costume.fullName,
                                                     asset.data, asset.len,
                                                     costume.rotationCenterX,
                                                     costume.rotationCenterY);
                    break;
                }
            }
        }
        for (auto &sound : sprite->sounds) {
            if (sound.fullName.empty()) continue;
            for (auto &asset : g_assets) {
                if (asset.name == sound.fullName) {
                    SoundPlayer::loadSoundFromMemory(sound.fullName,
                                                      asset.data, asset.len);
                    break;
                }
            }
        }
    }

    usb_ll_write("INIT\n");
    Scratch::turbo = true;
    Scratch::initializeScratchProject();

    scratch_running = true;
    xTaskCreatePinnedToCore(scratch_runtime_task, "scratch_rt", 65536, nullptr, 5, nullptr, 1);

    usb_ll_write("RUNNING\n");
    return true;
}

// ============================================================
// Main
// ============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Scratcher - ScratchEverywhere on ESP32-P4");
    ESP_LOGI(TAG, "Free PSRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    sprite_mutex = xSemaphoreCreateMutex();
    renderer = new SWRenderer();

#if USE_DSI_DISPLAY
    usb_ll_write("DSI_INIT_START\n");
    dsi_panel = dsi_display_init();
    if (dsi_panel) {
        usb_ll_write("DSI_INIT_OK\n");
    } else {
        usb_ll_write("DSI_INIT_FAIL\n");
    }
#else
    lcd_fb = (uint16_t *)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    usb_ll_write("LCD_INIT_START\n");
    lcd_panel = lcd_init();
    if (lcd_panel) {
        usb_ll_write("LCD_INIT_OK\n");
    } else {
        usb_ll_write("LCD_INIT_FAIL\n");
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(500));

    char line[256];
    while (true) {
        usb_ll_write("WAITING\n");
        free_assets();

        while (true) {
            if (!usb_ll_read_line(line, sizeof(line), 60000)) continue;

            if (strcmp(line, "LCD") == 0) {
                // Step-by-step LCD GPIO test
                const gpio_num_t SCLK = GPIO_NUM_21, MOSI_PIN = GPIO_NUM_38,
                                 CS = GPIO_NUM_19, DC = GPIO_NUM_35, RST = GPIO_NUM_37;
                for (auto p : {SCLK, MOSI_PIN, CS, DC, RST}) {
                    gpio_reset_pin(p);
                    gpio_set_direction(p, GPIO_MODE_OUTPUT);
                }
                gpio_set_level(CS, 1);
                gpio_set_level(SCLK, 0);

                auto spi_byte = [&](uint8_t b) {
                    for (int i = 7; i >= 0; i--) {
                        gpio_set_level(MOSI_PIN, (b >> i) & 1);
                        esp_rom_delay_us(100);
                        gpio_set_level(SCLK, 1);
                        esp_rom_delay_us(100);
                        gpio_set_level(SCLK, 0);
                        esp_rom_delay_us(100);
                    }
                };
                auto cmd = [&](uint8_t c) {
                    gpio_set_level(DC, 0); gpio_set_level(CS, 0);
                    spi_byte(c);
                    gpio_set_level(CS, 1);
                };
                auto dat = [&](uint8_t d) {
                    gpio_set_level(DC, 1); gpio_set_level(CS, 0);
                    spi_byte(d);
                    gpio_set_level(CS, 1);
                };

                // Test 1: RST toggle - screen should flicker
                usb_ll_write("T1:RST\n");
                gpio_set_level(RST, 0); vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(RST, 1); vTaskDelay(pdMS_TO_TICKS(500));
                usb_ll_write("T1:DONE\n");

                // Test 2: Init + display invert - white should become black
                usb_ll_write("T2:INIT\n");
                cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));  // SW reset
                cmd(0x11); vTaskDelay(pdMS_TO_TICKS(200));  // Sleep out
                cmd(0x29); vTaskDelay(pdMS_TO_TICKS(100));  // Display ON
                cmd(0x21); vTaskDelay(pdMS_TO_TICKS(100));  // Display INVERT
                usb_ll_write("T2:DONE\n");
                continue;
            }
            if (strcmp(line, "START") == 0) {
                load_and_run();
                // Wait for runtime task to finish before looping back
                while (scratch_running) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                ESP_LOGI(TAG, "Runtime task finished, ready for next project");
                break;
            }
            handle_command(line);
        }
    }
}
