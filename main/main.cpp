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
#include "esp_lcd_mipi_dsi.h"
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
#include "sd_storage.h"
#include "gamepad.h"
#include "es8388_audio.h"
#include "ui_menu.h"
#include <input.hpp>

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
extern "C" void render_set_input_callback(void (*cb)());

// ============================================================
// Gamepad → Scratch input callback
// ============================================================

#define STICK_DEADZONE 8000

static void gamepad_input_callback()
{
    GamepadState gs = gamepad_get_state();
    if (!gs.connected) return;

    uint16_t b = gs.buttons;
    if (b & XBOX_DPAD_UP)    Input::buttonPress("dpadUp");
    if (b & XBOX_DPAD_DOWN)  Input::buttonPress("dpadDown");
    if (b & XBOX_DPAD_LEFT)  Input::buttonPress("dpadLeft");
    if (b & XBOX_DPAD_RIGHT) Input::buttonPress("dpadRight");
    if (b & XBOX_A)          Input::buttonPress("A");
    if (b & XBOX_B)          Input::buttonPress("B");
    if (b & XBOX_X)          Input::buttonPress("X");
    if (b & XBOX_Y)          Input::buttonPress("Y");
    if (b & XBOX_LB)         Input::buttonPress("shoulderL");
    if (b & XBOX_RB)         Input::buttonPress("shoulderR");
    if (b & XBOX_START)      Input::buttonPress("start");
    if (b & XBOX_BACK)       Input::buttonPress("back");
    if (b & XBOX_LSTICK)     Input::buttonPress("LeftStickPressed");
    if (b & XBOX_RSTICK)     Input::buttonPress("RightStickPressed");
    if (gs.left_trigger > 128)  Input::buttonPress("LT");
    if (gs.right_trigger > 128) Input::buttonPress("RT");

    // Left stick → directional input
    if (gs.lx > STICK_DEADZONE)  Input::buttonPress("LeftStickRight");
    if (gs.lx < -STICK_DEADZONE) Input::buttonPress("LeftStickLeft");
    if (gs.ly > STICK_DEADZONE)  Input::buttonPress("LeftStickUp");
    if (gs.ly < -STICK_DEADZONE) Input::buttonPress("LeftStickDown");

    // Right stick
    if (gs.rx > STICK_DEADZONE)  Input::buttonPress("RightStickRight");
    if (gs.rx < -STICK_DEADZONE) Input::buttonPress("RightStickLeft");
    if (gs.ry > STICK_DEADZONE)  Input::buttonPress("RightStickUp");
    if (gs.ry < -STICK_DEADZONE) Input::buttonPress("RightStickDown");
}

// ============================================================
// Auto-map gamepad buttons based on keys used in Scratch project
// ============================================================

static void auto_map_gamepad()
{
    // Scan project for used keys (same logic as controlsMenu.cpp)
    std::vector<std::string> used_keys;
    for (auto &sprite : Scratch::sprites) {
        for (auto &block : sprite->blocks) {
            std::string key;
            if (block.opcode == "sensing_keypressed") {
                key = Input::convertToKey(Scratch::getInputValue(block, "KEY_OPTION", sprite));
            } else if (block.opcode == "event_whenkeypressed") {
                key = Input::convertToKey(Value(Scratch::getFieldValue(block, "KEY_OPTION")));
            } else continue;
            if (!key.empty() && key != "any" &&
                std::find(used_keys.begin(), used_keys.end(), key) == used_keys.end()) {
                used_keys.push_back(key);
            }
        }
    }

    if (used_keys.empty()) return;

    // Build smart mapping: arrow keys → D-pad, common keys → face buttons
    Input::inputControls.clear();

    // Always map D-pad to arrow keys and left stick
    Input::inputControls["dpadUp"] = "up arrow";
    Input::inputControls["dpadDown"] = "down arrow";
    Input::inputControls["dpadLeft"] = "left arrow";
    Input::inputControls["dpadRight"] = "right arrow";
    Input::inputControls["LeftStickUp"] = "up arrow";
    Input::inputControls["LeftStickDown"] = "down arrow";
    Input::inputControls["LeftStickLeft"] = "left arrow";
    Input::inputControls["LeftStickRight"] = "right arrow";

    // Collect non-arrow keys that need mapping to face buttons
    std::vector<std::string> remaining;
    for (auto &k : used_keys) {
        if (k == "up arrow" || k == "down arrow" || k == "left arrow" || k == "right arrow")
            continue;
        remaining.push_back(k);
    }

    // Priority mapping for common keys
    // "space" → A (most common action button in Scratch)
    // "w"/"up arrow" already handled by D-pad
    const char *face_buttons[] = {"A", "B", "X", "Y", "shoulderL", "shoulderR", "LT", "RT", "start", "back"};
    int face_idx = 0;

    // Map "space" to A first if used
    auto space_it = std::find(remaining.begin(), remaining.end(), "space");
    if (space_it != remaining.end()) {
        Input::inputControls["A"] = "space";
        remaining.erase(space_it);
        face_idx = 1; // skip A
    }

    // WASD → D-pad (alternative arrow mapping)
    auto map_wasd = [&](const char *key, const char *btn) {
        auto it = std::find(remaining.begin(), remaining.end(), std::string(key));
        if (it != remaining.end()) {
            Input::inputControls[btn] = key;
            remaining.erase(it);
        }
    };
    map_wasd("w", "dpadUp");
    map_wasd("s", "dpadDown");
    map_wasd("a", "dpadLeft");
    map_wasd("d", "dpadRight");

    // Map remaining keys to face buttons in order
    for (auto &k : remaining) {
        if (face_idx >= 10) break;
        Input::inputControls[face_buttons[face_idx]] = k;
        face_idx++;
    }

    // Log the mapping
    ESP_LOGI("automap", "Auto-mapped %zu keys from project:", used_keys.size());
    for (auto &[btn, key] : Input::inputControls) {
        ESP_LOGI("automap", "  %s → %s", btn.c_str(), key.c_str());
    }
}

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
                    float dir = sprite->rotation;
                    bool flip = false;
                    if (sprite->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) {
                        flip = (dir < 0);
                        dir = 90.0f; // no rotation
                    } else if (sprite->rotationStyle == Sprite::RotationStyle::NONE) {
                        dir = 90.0f; // no rotation
                    }
                    s_helper_state.renderer->drawSprite(cos.fullName,
                        sprite->xPosition, sprite->yPosition,
                        dir, sprite->size,
                        sprite->visible, sprite->ghostEffect,
                        sprite->brightnessEffect, flip);
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
                        float dir = sprite->rotation;
                        bool flip = false;
                        if (sprite->rotationStyle == Sprite::RotationStyle::LEFT_RIGHT) {
                            flip = (dir < 0);
                            dir = 90.0f;
                        } else if (sprite->rotationStyle == Sprite::RotationStyle::NONE) {
                            dir = 90.0f;
                        }
                        renderer->drawSprite(cos.fullName,
                                             sprite->xPosition, sprite->yPosition,
                                             dir, sprite->size,
                                             sprite->visible, sprite->ghostEffect,
                                             sprite->brightnessEffect, flip);
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
        render_set_input_callback(gamepad_input_callback);
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

    // Auto-map gamepad buttons based on keys used in project
    auto_map_gamepad();

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
    ui_download_update("Fetching info...", 0, 0);

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
    ui_download_update("Loading JSON...", 0, 0);
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
        ui_download_update("Assets", downloaded, total_assets);

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

// ============================================================
// Load game from SD card into runtime structures
// ============================================================

static void sd_asset_load_cb(const char *name, const uint8_t *data, size_t len, void *ctx)
{
    // Copy asset data (callback data is temporary)
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy) {
        memcpy(copy, data, len);
        g_assets.push_back({std::string(name), copy, len});
    }
}

static bool load_game_from_sd(const char *project_id)
{
    free_assets();

    size_t json_len = 0;
    char *json_str = sd_load_game(project_id, &json_len, sd_asset_load_cb, nullptr);
    if (!json_str) return false;

    if (g_project_json) delete g_project_json;
    g_project_json = new nlohmann::json();
    try {
        *g_project_json = nlohmann::json::parse(json_str, json_str + json_len);
    } catch (...) {
        ESP_LOGE(TAG, "Failed to parse project JSON from SD");
        heap_caps_free(json_str);
        delete g_project_json;
        g_project_json = nullptr;
        return false;
    }
    heap_caps_free(json_str);
    return true;
}

// ============================================================
// Save downloaded project to SD card
// ============================================================

static void save_current_project_to_sd(const char *project_id)
{
    if (!g_project_json || g_assets.empty()) return;

    std::string json_str = g_project_json->dump();

    // Build SdAsset array
    std::vector<SdAsset> sd_assets(g_assets.size());
    for (size_t i = 0; i < g_assets.size(); i++) {
        sd_assets[i].name = g_assets[i].name.c_str();
        sd_assets[i].data = g_assets[i].data;
        sd_assets[i].len = g_assets[i].len;
    }

    sd_save_game(project_id, project_id,
                 json_str.c_str(), json_str.size(),
                 sd_assets.data(), (int)sd_assets.size());
}

// ============================================================
// In-game gamepad polling for Start/Select
// ============================================================

static void game_overlay_check()
{
    GamepadState gs = gamepad_get_state();

    // Start → exit confirm
    if (gs.buttons & XBOX_START) {
        // Wait for release first
        while (gamepad_get_state().buttons & XBOX_START) vTaskDelay(pdMS_TO_TICKS(20));

        if (ui_show_exit_confirm()) {
            scratch_running = false;
        }
    }

    // Back/Select → show button map
    if (gs.buttons & XBOX_BACK) {
        while (gamepad_get_state().buttons & XBOX_BACK) vTaskDelay(pdMS_TO_TICKS(20));
        ui_show_button_map();
    }
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
    dsi_panel = dsi_display_init();
    es8388_audio_init();
#else
    lcd_fb = (uint16_t *)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    lcd_panel = lcd_init();
#endif

    vTaskDelay(pdMS_TO_TICKS(500));
    gamepad_init();
    sd_init();

#if USE_DSI_DISPLAY
    // Initialize LVGL menu system
    ui_init(dsi_panel);

    // Boot-time WiFi connection (try saved credentials)
    {
        wifi_init();
        char saved_ssid[64] = {}, saved_pass[128] = {};
        if (sd_load_wifi(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
            ui_show_wifi_connecting(saved_ssid);
            bool ok = wifi_connect(saved_ssid, saved_pass, 10000);
            ui_show_wifi_result(ok, saved_ssid);
            vTaskDelay(pdMS_TO_TICKS(ok ? 1000 : 2000));
        }
    }

    // Main loop: menu → game → menu → ...
    while (true) {
        MenuAction action = ui_menu_run();

        if (action == MenuAction::PLAY_FROM_SD) {
            const char *pid = ui_get_selected_project_id();
            ui_show_status("Loading...", pid);

            if (!load_game_from_sd(pid)) {
                ui_show_status("Load Failed", nullptr);
                vTaskDelay(pdMS_TO_TICKS(2000));
                ui_resume();
                continue;
            }
        } else if (action == MenuAction::PLAY_FROM_QR) {
            const char *pid = ui_get_selected_project_id();

            // Ensure WiFi is connected before download
            if (!wifi_is_connected()) {
                char ssid[64] = {}, pass[128] = {};
                if (sd_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
                    ui_show_wifi_connecting(ssid);
                    if (!wifi_connect(ssid, pass, 10000)) {
                        ui_show_wifi_result(false, ssid);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        ui_resume();
                        continue;
                    }
                    ui_show_wifi_result(true, ssid);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }

            ESP_LOGI(TAG, "DL: show_download_start");
            ui_show_download_start(pid);
            ESP_LOGI(TAG, "DL: calling qr_download_project");

            if (!qr_download_project(pid)) {
                ui_show_download_result(false);
                vTaskDelay(pdMS_TO_TICKS(2000));
                wifi_disconnect();
                ui_resume();
                continue;
            }

            ESP_LOGI(TAG, "DL: download complete, showing result");
            ui_show_download_result(true);
            vTaskDelay(pdMS_TO_TICKS(500));

            ESP_LOGI(TAG, "DL: wifi_disconnect");
            wifi_disconnect();

            ESP_LOGI(TAG, "DL: save_to_sd");
            save_current_project_to_sd(pid);
            ESP_LOGI(TAG, "DL: save_to_sd done");
        } else {
            continue;
        }

        // Suspend LVGL and run the game
        ui_show_status("Starting!", nullptr);
        vTaskDelay(pdMS_TO_TICKS(500));
        ui_suspend();

        // Clear DPI framebuffer to black (remove LVGL UI residue)
        {
            void *fb0 = nullptr;
            if (esp_lcd_dpi_panel_get_frame_buffer(dsi_panel, 1, &fb0) == ESP_OK && fb0) {
                memset(fb0, 0, DSI_LCD_W * DSI_LCD_H * 2);
                esp_cache_msync(fb0, DSI_LCD_W * DSI_LCD_H * 2,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            }
        }

        if (load_and_run()) {
            // Game loop: check for Start/Select overlays
            while (scratch_running) {
                game_overlay_check();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // Game ended — clean up and return to menu
        free_assets();
        // ui_resume() happens at top of next ui_menu_run() iteration
    }

#else
    // USB Serial Mode (SPI LCD fallback)
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
#endif
}
