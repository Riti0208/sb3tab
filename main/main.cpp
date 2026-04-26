#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

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
#include "camera_qr.h"
#include "wifi_manager.h"
#include "dsi_modal.h"

// Build for DSI (Tab5) or SPI LCD
#define USE_DSI_DISPLAY 1

// Enable QR code scanning mode (camera + WiFi download)
#define USE_QR_MODE 1

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
    int retries = 0;
    while (remaining > 0) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            int written = usb_serial_jtag_ll_write_txfifo(p, remaining);
            usb_serial_jtag_ll_txfifo_flush();
            p += written;
            remaining -= written;
            retries = 0;
        } else {
            retries++;
            if (retries > 50) break;  // Don't block if host isn't reading
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
extern "C" void render_set_costume_size_callback(
    bool (*)(const char *name, int *w, int *h)
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
static bool costume_size_cb(const char *name, int *w, int *h) {
    if (renderer) return renderer->getCostumeSize(name, *w, *h);
    return false;
}

// ============================================================
// Dual-core render helper (Core 0)
// ============================================================

static SemaphoreHandle_t s_render_start = NULL;  // Main → Helper: start work
static SemaphoreHandle_t s_render_done = NULL;   // Helper → Main: work complete
static SemaphoreHandle_t s_spr_start = NULL;     // Main → Helper: start sprite rendering
static SemaphoreHandle_t s_spr_done = NULL;      // Helper → Main: sprites done

// Shared state for helper task
struct RenderHelperState {
    SWRenderer *renderer;
    std::vector<Sprite *> *sprites;
    int phase;  // 0=clear, 1=sprites
};
static RenderHelperState s_helper_state;

static void render_helper_task(void *param) {
    while (true) {
        // Phase 1: Clear framebuffer (parallel with step on Core 1)
        xSemaphoreTake(s_render_start, portMAX_DELAY);
        if (s_helper_state.renderer) {
            bool cleared = false;
            if (Scratch::stageSprite) {
                int cosIdx = Scratch::stageSprite->currentCostume;
                if (cosIdx >= 0 && cosIdx < (int)Scratch::stageSprite->costumes.size()) {
                    cleared = s_helper_state.renderer->clearDirty();
                    if (!cleared) {
                        cleared = s_helper_state.renderer->drawBackdropFull(
                            Scratch::stageSprite->costumes[cosIdx].fullName);
                    }
                }
            }
            if (!cleared) s_helper_state.renderer->clear();
        }
        xSemaphoreGive(s_render_done);

        // Phase 2: Render even-indexed sprites (parallel with odd on Core 1)
        xSemaphoreTake(s_spr_start, portMAX_DELAY);
        if (s_helper_state.renderer && s_helper_state.sprites) {
            int idx = 0;
            for (auto &sprite : *s_helper_state.sprites) {
                if (sprite->isStage || !sprite->visible) continue;
                int cosIdx = sprite->currentCostume;
                if (cosIdx < 0 || cosIdx >= (int)sprite->costumes.size()) continue;
                if (idx % 2 == 0) {  // Even sprites on Core 0
                    const auto &cos = sprite->costumes[cosIdx];
                    s_helper_state.renderer->drawSprite(cos.fullName,
                        sprite->xPosition, sprite->yPosition,
                        sprite->rotation, sprite->size,
                        sprite->visible, sprite->ghostEffect,
                        sprite->brightnessEffect);
                }
                idx++;
            }
        }
        xSemaphoreGive(s_spr_done);
    }
}

// ============================================================
// Scratch Runtime Task (Core 1)
// ============================================================

static int s_frame_count = 0;
static int64_t s_total_step = 0, s_total_render = 0, s_total_lcd = 0;

static void scratch_runtime_task(void *param)
{
    // Create render helper on Core 0
    s_render_start = xSemaphoreCreateBinary();
    s_render_done = xSemaphoreCreateBinary();
    s_spr_start = xSemaphoreCreateBinary();
    s_spr_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(render_helper_task, "render_helper", 8192, nullptr, 5, nullptr, 0);

    // Kick off first clear before entering loop
    s_helper_state.renderer = renderer;
    s_helper_state.sprites = &Scratch::sprites;
    xSemaphoreGive(s_render_start);

    while (scratch_running) {
        xSemaphoreTake(sprite_mutex, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        auto [running, restart] = Scratch::stepScratchProject();
        int64_t t1 = esp_timer_get_time();

        if (renderer) {
            // Wait for clear (started at end of previous frame or initial)
            xSemaphoreTake(s_render_done, portMAX_DELAY);
            int64_t r1 = esp_timer_get_time();

            // Composite pen layer
            renderer->compositePenLayer();

            // Draw sprites split across cores
            xSemaphoreGive(s_spr_start);
            {
                int idx = 0;
                for (auto &sprite : Scratch::sprites) {
                    if (sprite->isStage || !sprite->visible) continue;
                    int cosIdx = sprite->currentCostume;
                    if (cosIdx < 0 || cosIdx >= (int)sprite->costumes.size()) continue;
                    if (idx % 2 == 1) {
                        const auto &cos = sprite->costumes[cosIdx];
                        renderer->drawSprite(cos.fullName,
                                             sprite->xPosition, sprite->yPosition,
                                             sprite->rotation, sprite->size,
                                             sprite->visible, sprite->ghostEffect,
                                             sprite->brightnessEffect);
                    }
                    idx++;
                }
            }
            xSemaphoreTake(s_spr_done, portMAX_DELAY);
            int64_t r4 = esp_timer_get_time();

            // Speech bubbles
            SpeechManager *sm = Render::getSpeechManager();
            if (sm) {
                static_cast<SpeechManagerHeadless *>(sm)->renderToFramebuffer(
                    renderer->getFramebuffer(), STAGE_W, STAGE_H,
                    Scratch::projectWidth, Scratch::projectHeight);
            }

            // Variable/list monitors
            g_headless_fb.fb = renderer->getFramebuffer();
            g_headless_fb.width = STAGE_W;
            g_headless_fb.height = STAGE_H;
            Render::renderMonitors();

            // Swap framebuffer and push to display
#if USE_DSI_DISPLAY
            if (dsi_panel) {
                uint8_t *completedFb = renderer->swapFramebuffer();
                dsi_display_update(dsi_panel, completedFb, STAGE_W, STAGE_H);
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

            // Start NEXT frame's clear immediately (pipelined: runs during vTaskDelay + next step)
            xSemaphoreGive(s_render_start);

            s_frame_count++;
            // Frame stats disabled - usb_ll_write blocks when host doesn't read, freezing render loop
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
        render_set_costume_size_callback(costume_size_cb);
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
                    // Set sprite dimensions for collision/bounce detection
                    int cw, ch;
                    if (renderer->getCostumeSize(costume.fullName, cw, ch)) {
                        sprite->spriteWidth = cw;
                        sprite->spriteHeight = ch;
                    }
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
    Scratch::accurateCollision = false;  // No bitmask images on headless; use fast AABB collision
    Scratch::initializeScratchProject();

    scratch_running = true;
    xTaskCreatePinnedToCore(scratch_runtime_task, "scratch_rt", 65536, nullptr, 5, nullptr, 1);

    usb_ll_write("RUNNING\n");
    return true;
}

// ============================================================
// QR mode: download project via WiFi
// ============================================================

#if USE_QR_MODE && USE_DSI_DISPLAY

// Simple text overlay on DPI framebuffer (RGB565, portrait 720x1280)
// Draws white text on semi-transparent black bar at bottom
static bool qr_download_project(const char *project_id)
{
    usb_ll_write("DL_START\n");
    dsi_modal_show(dsi_panel, "Fetching info...", project_id);

    // Step 1: Get project token from API
    char url[256];
    snprintf(url, sizeof(url), "https://api.scratch.mit.edu/projects/%s", project_id);

    size_t api_len = 0;
    uint8_t *api_data = wifi_http_get(url, &api_len);
    if (!api_data) {
        usb_ll_write("ERR:api fetch\n");
        return false;
    }

    // Parse API response for project_token
    nlohmann::json api_json;
    try {
        api_json = nlohmann::json::parse(api_data, api_data + api_len);
    } catch (...) {
        usb_ll_write("ERR:api json\n");
        heap_caps_free(api_data);
        return false;
    }
    heap_caps_free(api_data);

    std::string token;
    if (api_json.contains("project_token")) {
        token = api_json["project_token"].get<std::string>();
    }

    // Step 2: Download project.json
    dsi_modal_show(dsi_panel, "Loading JSON...", nullptr);
    snprintf(url, sizeof(url), "https://projects.scratch.mit.edu/%s%s%s",
             project_id, token.empty() ? "" : "?token=", token.c_str());

    size_t json_len = 0;
    uint8_t *json_data = wifi_http_get(url, &json_len, [](size_t rx, size_t total) {
        if (total > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "JSON: %zu/%zu (%d%%)\n", rx, total, (int)(rx * 100 / total));
            usb_ll_write(msg);
        }
    });
    if (!json_data) {
        usb_ll_write("ERR:json fetch\n");
        return false;
    }

    // Parse project JSON
    if (g_project_json) delete g_project_json;
    g_project_json = new nlohmann::json();
    try {
        *g_project_json = nlohmann::json::parse(json_data, json_data + json_len);
    } catch (...) {
        usb_ll_write("ERR:json parse\n");
        heap_caps_free(json_data);
        delete g_project_json;
        g_project_json = nullptr;
        return false;
    }
    heap_caps_free(json_data);
    usb_ll_write("JSON_OK\n");

    // Step 3: Extract asset list and download each
    std::vector<std::string> asset_names;
    for (auto &target : (*g_project_json)["targets"]) {
        for (auto &costume : target["costumes"]) {
            std::string md5ext;
            if (costume.contains("md5ext")) {
                md5ext = costume["md5ext"].get<std::string>();
            } else {
                md5ext = costume.value("assetId", "") + "." + costume.value("dataFormat", "svg");
            }
            if (!md5ext.empty()) asset_names.push_back(md5ext);
        }
        for (auto &sound : target["sounds"]) {
            std::string md5ext;
            if (sound.contains("md5ext")) {
                md5ext = sound["md5ext"].get<std::string>();
            } else {
                md5ext = sound.value("assetId", "") + "." + sound.value("dataFormat", "wav");
            }
            if (!md5ext.empty()) asset_names.push_back(md5ext);
        }
    }

    // Remove duplicates
    std::sort(asset_names.begin(), asset_names.end());
    asset_names.erase(std::unique(asset_names.begin(), asset_names.end()), asset_names.end());

    int total_assets = (int)asset_names.size();
    int downloaded = 0;

    for (auto &name : asset_names) {
        dsi_modal_progress(dsi_panel, "Assets", downloaded, total_assets);

        snprintf(url, sizeof(url),
                 "https://assets.scratch.mit.edu/internalapi/asset/%s/get/", name.c_str());

        size_t asset_len = 0;
        uint8_t *asset_data = wifi_http_get(url, &asset_len);

        if (!asset_data) {
            char msg[128];
            snprintf(msg, sizeof(msg), "WARN:asset skip %s\n", name.c_str());
            usb_ll_write(msg);
            continue;
        }

        g_assets.push_back({name, asset_data, asset_len});
        downloaded++;

        char msg[128];
        snprintf(msg, sizeof(msg), "ASSET %d/%d: %s (%zu B)\n",
                 downloaded, total_assets, name.c_str(), asset_len);
        usb_ll_write(msg);
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "DL_DONE: %d assets\n", downloaded);
    usb_ll_write(msg);
    return true;
}

#endif // USE_QR_MODE && USE_DSI_DISPLAY

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

#if USE_QR_MODE && USE_DSI_DISPLAY
    // ============================================================
    // QR Scan Mode: Camera → WiFi QR → Project QR → Download → Run
    // ============================================================
    {
        usb_ll_write("QR_MODE_START\n");

        // Initialize WiFi subsystem
        wifi_init();

        // Initialize camera
        if (!camera_init()) {
            usb_ll_write("ERR:camera init failed, falling back to USB mode\n");
            goto usb_mode;
        }

        char qr_buf[512];

        // Phase 1: Scan WiFi QR code (also accepts USB serial: WIFI:SSID:PASSWORD)
        usb_ll_write("SCAN_WIFI_QR\n");
        {
            char usb_line[256];
            auto try_wifi_connect = [&](char *ssid, char *password) -> bool {
                char msg[128];
                snprintf(msg, sizeof(msg), "WIFI_CONNECTING: %.64s\n", ssid);
                usb_ll_write(msg);
                dsi_modal_show(dsi_panel, "Connecting...", ssid);

                if (wifi_connect(ssid, password, 15000)) {
                    usb_ll_write("WIFI_OK\n");
                    dsi_modal_show(dsi_panel, "WiFi OK!", ssid);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    return true;
                } else {
                    usb_ll_write("WIFI_FAIL\n");
                    dsi_modal_show(dsi_panel, "WiFi Failed", "Try again...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    return false;
                }
            };

            bool connected = false;
            int wifi_attempts = 0;
            while (!connected) {
                // Check QR camera
                if (camera_scan_qr(qr_buf, sizeof(qr_buf), dsi_panel, "Scan WiFi QR")) {
                    if (qr_buf[0] == 'W' && qr_buf[1] == ':') {
                        char *ssid = qr_buf + 2;
                        char *newline = strchr(ssid, '\n');
                        char *password = (char *)"";
                        if (newline) { *newline = '\0'; password = newline + 1; }
                        connected = try_wifi_connect(ssid, password);
                        if (!connected) {
                            wifi_attempts++;
                            if (wifi_attempts >= 3) {
                                usb_ll_write("WIFI_GIVE_UP: falling back to USB mode\n");
                                dsi_modal_show(dsi_panel, "WiFi failed 3x", "USB mode...");
                                vTaskDelay(pdMS_TO_TICKS(2000));
                                camera_deinit();
                                goto usb_mode;
                            }
                        }
                    }
                }
                // Check USB serial: "WIFI:SSID:PASSWORD"
                if (!connected && usb_ll_read_line(usb_line, sizeof(usb_line), 100)) {
                    if (strncmp(usb_line, "WIFI:", 5) == 0) {
                        char *ssid = usb_line + 5;
                        char *colon = strchr(ssid, ':');
                        char *password = (char *)"";
                        if (colon) { *colon = '\0'; password = colon + 1; }
                        connected = try_wifi_connect(ssid, password);
                    } else if (strncmp(usb_line, "USB", 3) == 0) {
                        usb_ll_write("USB_MODE\n");
                        camera_deinit();
                        goto usb_mode;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // Phase 2: Scan Project QR codes in a loop
        while (true) {
            usb_ll_write("SCAN_PROJECT_QR\n");

            bool project_found = false;
            while (!project_found) {
                if (camera_scan_qr(qr_buf, sizeof(qr_buf), dsi_panel, "Scan Project QR")) {
                    // Check for Project QR: "S:PROJECT_ID"
                    if (qr_buf[0] == 'S' && qr_buf[1] == ':') {
                        const char *project_id = qr_buf + 2;
                        char msg[128];
                        snprintf(msg, sizeof(msg), "PROJECT: %.64s\n", project_id);
                        usb_ll_write(msg);

                        // Stop camera and show download modal
                        camera_deinit();
                        dsi_modal_show(dsi_panel, "Downloading...", project_id);

                        // Download project
                        free_assets();
                        if (qr_download_project(project_id)) {
                            // Disconnect WiFi before running (SDIO conflicts with dual-core rendering)
                            wifi_disconnect();
                            dsi_modal_show(dsi_panel, "Starting!", nullptr);
                            vTaskDelay(pdMS_TO_TICKS(500));

                            // Run project
                            load_and_run();

                            // Wait for runtime to finish
                            while (scratch_running) {
                                vTaskDelay(pdMS_TO_TICKS(100));
                            }
                            usb_ll_write("PROJECT_DONE\n");
                        } else {
                            usb_ll_write("DL_FAIL\n");
                            dsi_modal_show(dsi_panel, "Download Failed", "Try again...");
                            vTaskDelay(pdMS_TO_TICKS(3000));
                        }

                        // Restart camera for next QR scan
                        camera_init();
                        project_found = true;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
    }
usb_mode:
#endif // USE_QR_MODE && USE_DSI_DISPLAY

    // ============================================================
    // USB Serial Mode (fallback / SPI LCD mode)
    // ============================================================
    char line[256];
    while (true) {
        usb_ll_write("WAITING\n");
        free_assets();

        while (true) {
            if (!usb_ll_read_line(line, sizeof(line), 60000)) continue;

            if (strcmp(line, "START") == 0) {
                load_and_run();
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
