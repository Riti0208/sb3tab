#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
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
#include <image.hpp>
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
#include "input_device.h"
#include "input_xbox.h"
#include "input_gpio.h"
#include "touch_input.h"
#include "es8388_audio.h"
#include "ui_menu.h"
#include "i18n.h"
#include "battery_monitor.h"
#include "time_sync.h"
#include "driver/i2c_master.h"
#include <input.hpp>

extern i2c_master_bus_handle_t g_i2c_bus;

// Build for DSI (Tab5) or SPI LCD
#define USE_DSI_DISPLAY 1

// Enable QR code scanning mode (camera + WiFi download)
#define USE_QR_MODE 1

static const char *TAG = "scratcher";

extern BlockResult nopBlock(Block &, Sprite *, bool *, bool);
__attribute__((used)) static auto *_force_blocks = &nopBlock;

static volatile bool scratch_running = false;
static volatile bool scratch_paused = false;
static SemaphoreHandle_t s_runtime_done = nullptr;
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
static std::string g_project_title;

struct AssetEntry {
    std::string name;
    uint8_t *data;
    size_t len;
};
static std::vector<AssetEntry> g_assets;

// When non-empty, the runtime asset resolvers (Image::setMemoryProvider and
// the costume/sound loaders) read each asset on demand from
// /sd/games/<g_assets_sd_project_id>/assets/<name> instead of looking up
// g_assets in PSRAM. Set after a streaming QR download finishes; cleared on
// USB-fed loads where everything stays resident in g_assets.
static std::string g_assets_sd_project_id;

static void free_assets() {
    for (auto &a : g_assets) {
        if (a.data) heap_caps_free(a.data);
    }
    g_assets.clear();
    g_assets_sd_project_id.clear();
    g_project_title.clear();
    if (g_project_json) { delete g_project_json; g_project_json = nullptr; }
}

#if USE_DSI_DISPLAY
// Unified DSI loading-modal helpers. Once LVGL is suspended (ui_suspend) and
// both DPI framebuffers are cleared, every loading phase — Wi-Fi connect,
// JSON download, asset download, sound/costume rasterize — pushes its status
// through these so the user sees one continuous modal instead of LVGL
// screens flashing into a black screen and back.
static int64_t s_loading_last_us = 0;

// Scratch blue — same shade the modal header uses, so the backdrop and the
// header read as one continuous block of color with the white card sitting
// in the middle.
static const uint16_t LOADING_BG_BLUE =
    (uint16_t)(((0x4C >> 3) << 11) | ((0x97 >> 2) << 5) | (0xFF >> 3));

static void loading_begin() {
    dsi_modal_set_bg(LOADING_BG_BLUE);
}

static void loading_end() {
    dsi_modal_set_bg(0);
}

static void loading_show(const char *title, const char *detail) {
    if (!dsi_panel) return;
    dsi_modal_show(dsi_panel, title, detail);
    s_loading_last_us = esp_timer_get_time();
}

static void loading_progress(const char *title, int current, int total) {
    if (!dsi_panel) return;
    int64_t now = esp_timer_get_time();
    bool terminal = (current == 0) || (total > 0 && current >= total);
    // Throttle to ~10 fps; each call writes ~1.8 MB into PSRAM and waits a
    // vsync, so an unthrottled loop would dominate load time on big projects.
    if (!terminal && (now - s_loading_last_us) < 100000) return;
    dsi_modal_progress(dsi_panel, title, current, total);
    s_loading_last_us = now;
}
#endif

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

// Override the weak symbol in scratch_core so its pixel-perfect collision can
// pull RGBA pixels from our SWRenderer (headless build has no Image cache).
extern "C" const uint8_t *render_get_costume_rgba(const char *name, int *w, int *h) {
    if (!renderer || !name || !w || !h) return nullptr;
    return renderer->getCostumeRGBA(name, *w, *h);
}

extern "C" {
int collision_g_call_count();
int collision_g_cache_hit();
int collision_g_inner_iters();
int64_t collision_g_total_us();
void collision_reset_counters();
}

// ============================================================
// Gamepad → Scratch input callback
// ============================================================

#define STICK_DEADZONE 8000

// Bridge: hardware InputDev state → Scratch core's named buttons.
// The Scratch runtime looks up these names in Input::inputControls (populated
// by auto_map_input() below) to know which key each button represents.
//
// Also injects touch panel state into Input::mousePointer so blocks like
// mousex/mousey/mousedown, "touching mouse-pointer", and click hat events
// (event_whenthisspriteclicked) work — that's the entire mouse/click surface
// the Scratch runtime exposes.
static void input_to_scratch_callback()
{
    // --- touch → Input::mousePointer ---
    touch_stage_t t;
    if (touch_input_get_stage(&t)) {
        Input::mousePointer.x = t.stage_x;
        Input::mousePointer.y = t.stage_y;
        Input::mousePointer.isPressed = t.pressed;
        Input::mousePointer.isMoving = t.pressed;
    }

    InputDev::State s = InputDev::get();
    if (!s.any_connected) return;

    using namespace InputDev;
    uint32_t b = s.buttons;
    if (b & DPAD_UP)    Input::buttonPress("dpadUp");
    if (b & DPAD_DOWN)  Input::buttonPress("dpadDown");
    if (b & DPAD_LEFT)  Input::buttonPress("dpadLeft");
    if (b & DPAD_RIGHT) Input::buttonPress("dpadRight");
    if (b & A)          Input::buttonPress("A");
    if (b & B)          Input::buttonPress("B");
    if (b & X)          Input::buttonPress("X");
    if (b & Y)          Input::buttonPress("Y");
    if (b & LB)         Input::buttonPress("shoulderL");
    if (b & RB)         Input::buttonPress("shoulderR");
    if (b & START)      Input::buttonPress("start");
    if (b & BACK)       Input::buttonPress("back");
    if (b & LSTICK)     Input::buttonPress("LeftStickPressed");
    if (b & RSTICK)     Input::buttonPress("RightStickPressed");
    if (b & LT)         Input::buttonPress("LT");
    if (b & RT)         Input::buttonPress("RT");

    if (s.lx > STICK_DEADZONE)  Input::buttonPress("LeftStickRight");
    if (s.lx < -STICK_DEADZONE) Input::buttonPress("LeftStickLeft");
    if (s.ly > STICK_DEADZONE)  Input::buttonPress("LeftStickUp");
    if (s.ly < -STICK_DEADZONE) Input::buttonPress("LeftStickDown");

    if (s.rx > STICK_DEADZONE)  Input::buttonPress("RightStickRight");
    if (s.rx < -STICK_DEADZONE) Input::buttonPress("RightStickLeft");
    if (s.ry > STICK_DEADZONE)  Input::buttonPress("RightStickUp");
    if (s.ry < -STICK_DEADZONE) Input::buttonPress("RightStickDown");
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
static SemaphoreHandle_t s_helper_exited = NULL; // Helper → Main: task is exiting
static volatile bool s_helper_run = false;

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
        if (!s_helper_run) break;
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
        if (!s_helper_run) break;
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
    xSemaphoreGive(s_helper_exited);
    vTaskDeleteWithCaps(nullptr);  // task created with xTaskCreatePinnedToCoreWithCaps
}

// ============================================================
// Scratch Runtime Task (Core 1)
// ============================================================

static int s_frame_count = 0;
static int64_t s_total_step = 0, s_total_render = 0, s_total_lcd = 0;

// Tick rate is driven by Scratch::FPS (set by parser from project config,
// defaults to 30). Turbo mode in real Scratch doesn't change tick rate — it
// disables yields between repeat iterations within a tick. We approximate by
// uncapping the runtime loop when turbo is on, letting scripts step as fast
// as the CPU allows. (Proper turbo semantics — multiple iterations per tick
// without yielding — would need scratch_core changes; not implemented yet.)

static void scratch_runtime_task(void *param)
{
    s_frame_count = 0;
    ESP_LOGI(TAG, "RT: task started");
    // Create render helper on Core 0
    s_render_start = xSemaphoreCreateBinary();
    s_render_done = xSemaphoreCreateBinary();
    s_spr_start = xSemaphoreCreateBinary();
    s_spr_done = xSemaphoreCreateBinary();
    s_helper_exited = xSemaphoreCreateBinary();
    s_helper_run = true;
    // PNG decoding (stbi__compute_huffman_codes) puts ~3 KB of huffman
    // tables on the stack; the decoder runs on whichever core happens to
    // request a not-yet-cached costume, so render_helper needs the same
    // headroom as the scratch_rt task. Allocate the 32KB stack in PSRAM
    // (internal RAM is too tight — boot leaves only ~120 KB largest free).
    xTaskCreatePinnedToCoreWithCaps(render_helper_task, "render_helper", 32768,
                                     nullptr, 5, nullptr, 0, MALLOC_CAP_SPIRAM);

    // Kick off first clear before entering loop
    s_helper_state.renderer = renderer;
    s_helper_state.sprites = &Scratch::sprites;
    xSemaphoreGive(s_render_start);

    while (scratch_running) {
        // Paused (overlay modal showing): skip step + render so nothing touches the DPI FB
        if (scratch_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        xSemaphoreTake(sprite_mutex, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        // Wrap stepScratchProject in a catch — projects with lots of clones
        // (e.g. Grillin: 11 control_create_clone_of blocks) can hit
        // std::bad_alloc deep in Sprite::operator= when PSRAM is fragmented.
        // Without this catch the exception propagates to terminate() and the
        // whole device reboots. Skipping a single tick is far better than
        // losing the whole game.
        std::pair<bool, bool> step_result = {true, false};
        try {
            step_result = Scratch::stepScratchProject();
        } catch (const std::bad_alloc &) {
            ESP_LOGW(TAG, "step: bad_alloc (PSRAM fragmented, free=%u largest=%u) — skip frame",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        }
        auto [running, restart] = step_result;
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

            // Swap framebuffer and push to display (skipped while paused so overlay stays visible)
#if USE_DSI_DISPLAY
            if (dsi_panel) {
                uint8_t *completedFb = renderer->swapFramebuffer();
                if (!scratch_paused) {
                    dsi_display_update(dsi_panel, completedFb, STAGE_W, STAGE_H);
                }
            }
#else
            if (lcd_panel && lcd_fb && !scratch_paused) {
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
            // Accumulate frame phase totals
            s_total_step += t1 - t0;
            s_total_render += r4 - r1;
            s_total_lcd += t3 - r4;
            if ((s_frame_count % 60) == 1) {
                int calls = collision_g_call_count();
                int hits = collision_g_cache_hit();
                int64_t us = collision_g_total_us();
                ESP_LOGI(TAG, "RT: frame %d  step=%lldus render=%lldus lcd=%lldus  collision: calls=%d hits=%d total=%lldus",
                         s_frame_count, s_total_step / 60, s_total_render / 60, s_total_lcd / 60,
                         calls, hits, us);
                collision_reset_counters();
                s_total_step = 0;
                s_total_render = 0;
                s_total_lcd = 0;
            }
            // Frame stats disabled - usb_ll_write blocks when host doesn't read, freezing render loop
        }
        xSemaphoreGive(sprite_mutex);

        if (!running) break;
        // Throttle to the project's declared FPS (parser default 30Hz).
        // We forced Scratch::turbo above so scripts step every iteration, so
        // the loop period is the effective tick rate.
        const int fps = Scratch::FPS > 0 ? Scratch::FPS : 30;
        const int64_t frame_us = 1000000 / fps;
        int64_t elapsed = esp_timer_get_time() - t0;
        int64_t sleep_us = frame_us - elapsed;
        if (sleep_us > 1000) {
            vTaskDelay(pdMS_TO_TICKS((int)(sleep_us / 1000)));
        } else {
            vTaskDelay(1);
        }
    }
    scratch_running = false;
    ESP_LOGI(TAG, "RT: exiting (frames=%d)", s_frame_count);

    // Shut down render helper cleanly so re-launching the game works
    s_helper_run = false;
    xSemaphoreGive(s_render_start);   // unblock if waiting on phase 1
    xSemaphoreGive(s_spr_start);      // unblock if waiting on phase 2
    xSemaphoreTake(s_helper_exited, portMAX_DELAY);

    vSemaphoreDelete(s_render_start); s_render_start = nullptr;
    vSemaphoreDelete(s_render_done);  s_render_done = nullptr;
    vSemaphoreDelete(s_spr_start);    s_spr_start = nullptr;
    vSemaphoreDelete(s_spr_done);     s_spr_done = nullptr;
    vSemaphoreDelete(s_helper_exited); s_helper_exited = nullptr;

    if (s_runtime_done) xSemaphoreGive(s_runtime_done);
    vTaskDeleteWithCaps(nullptr);
}

// ============================================================
// Load and run project
// ============================================================

static bool load_and_run()
{
    ESP_LOGW("BLUE_DBG", "===== load_and_run START =====");
    if (!g_project_json) {
        usb_ll_write("ERR:no json\n");
        return false;
    }

    if (scratch_running) {
        scratch_running = false;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_task_wdt_deinit();

    // Belt-and-suspenders: the menu→game path already calls
    // Scratch::cleanupScratchProject() before re-entering load_and_run, but
    // we keep this call in case load_and_run is invoked from elsewhere
    // (USB serial mode) without going through the menu cleanup flow.
    Scratch::cleanupScratchProject();
    SoundPlayer::cleanupAudio();

    // Initialize headless renderer (creates WindowHeadless)
    static bool render_initialized = false;
    if (!render_initialized) {
        Render::Init();
        render_set_pen_callbacks(pen_init_cb, pen_clear_cb, pen_line_cb, pen_dot_cb, pen_stamp_cb);
        render_set_costume_size_callback(costume_size_cb);
        render_set_input_callback(input_to_scratch_callback);
        // Serve image bytes either from g_assets (USB-fed projects) or
        // directly from SD (streamed-download projects, where g_assets is
        // empty to keep PSRAM peak ≈ one asset).
        Image::setMemoryProvider([](const std::string &filePath)
            -> nonstd::expected<std::vector<unsigned char>, std::string> {
            std::string name = filePath;
            const std::string prefix = "project/";
            if (name.compare(0, prefix.size(), prefix) == 0) {
                name = name.substr(prefix.size());
            }
            for (auto &asset : g_assets) {
                if (asset.name == name) {
                    return std::vector<unsigned char>(asset.data, asset.data + asset.len);
                }
            }
            if (!g_assets_sd_project_id.empty()) {
                size_t alen = 0;
                uint8_t *adata = sd_read_asset(g_assets_sd_project_id.c_str(),
                                               name.c_str(), &alen);
                if (adata) {
                    std::vector<unsigned char> v(adata, adata + alen);
                    heap_caps_free(adata);
                    return v;
                }
            }
            return nonstd::make_unexpected("not in memory: " + filePath);
        });
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

    // Load costumes and sounds. Resolve each asset by name from either
    // g_assets (USB-resident) or SD (streamed-download). The `from_sd` flag
    // tells the consumer whether the buffer is about to be freed: for SD
    // we MUST force a copy because audio's fast path borrows the source
    // pointer for the sound's lifetime.
    auto resolve_asset = [](const std::string &name,
                            std::function<void(const uint8_t *, size_t, bool)> use)
    {
        for (auto &asset : g_assets) {
            if (asset.name == name) {
                use(asset.data, asset.len, /*from_sd=*/false);
                return true;
            }
        }
        if (!g_assets_sd_project_id.empty()) {
            size_t alen = 0;
            uint8_t *adata = sd_read_asset(g_assets_sd_project_id.c_str(),
                                            name.c_str(), &alen);
            if (adata) {
                use(adata, alen, /*from_sd=*/true);
                heap_caps_free(adata);
                return true;
            }
        }
        return false;
    };

    // Phase 1: load ALL sounds first, BEFORE any costume RGBA fragments PSRAM.
    // The BGM PCM can be multi-MB (a 1.3 MB MP3 expands to ~7 MB mono int16),
    // and on a project re-load PSRAM fills with 100 KB-class costume buffers
    // that quickly chew the largest contiguous block from ~7 MB down to ~1 MB
    // — too small to fit the BGM and the alloc fails. Loading sounds first
    // lets the largest sound claim a clean contiguous block before fragmentation.
#if USE_DSI_DISPLAY
    int total_sounds = 0;
    for (auto &sprite : Scratch::sprites) total_sounds += (int)sprite->sounds.size();
    int sound_idx = 0;
    if (total_sounds > 0) loading_progress(tr(STR_LOADING_SOUNDS), 0, total_sounds);
#endif
    for (auto &sprite : Scratch::sprites) {
        for (auto &sound : sprite->sounds) {
            if (sound.fullName.empty()) continue;
            bool ok = resolve_asset(sound.fullName, [&](const uint8_t *data, size_t len, bool from_sd) {
                SoundPlayer::loadSoundFromMemory(sound.fullName, data, len,
                                                  /*force_copy=*/from_sd);
            });
            if (!ok) {
                ESP_LOGW(TAG, "sound asset MISSING: '%s' (sprite='%s')",
                         sound.fullName.c_str(), sprite->name.c_str());
            }
            // Yield CPU/PSRAM bandwidth between each sound decode (debug:
            // testing whether DPI underrun-without-detection is the cause
            // of blue-flash-during-load).
            vTaskDelay(1);
#if USE_DSI_DISPLAY
            sound_idx++;
            loading_progress(tr(STR_LOADING_SOUNDS), sound_idx, total_sounds);
#endif
        }
    }

    // After sounds claim their PSRAM, shrink the costume LRU budget to what's
    // actually available so eviction happens during the load loop (predictable)
    // rather than at first-display time (where a re-decode failure would leave
    // a sprite invisible). 2 MB safety margin keeps room for runtime
    // allocations (pen layer, JPEG encoders, transient HTTP buffers).
    if (renderer) {
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const size_t SAFETY_MARGIN = 2 * 1024 * 1024;
        const size_t MIN_BUDGET = 2 * 1024 * 1024;
        const size_t MAX_BUDGET = 8 * 1024 * 1024;
        size_t budget = (psram_free > SAFETY_MARGIN) ? psram_free - SAFETY_MARGIN : MIN_BUDGET;
        if (budget > MAX_BUDGET) budget = MAX_BUDGET;
        if (budget < MIN_BUDGET) budget = MIN_BUDGET;
        renderer->setCostumeBudgetBytes(budget);
        ESP_LOGI(TAG, "Costume LRU budget set to %zu KB (PSRAM free=%zu KB)",
                 budget / 1024, psram_free / 1024);
    }

    // Phase 2: costumes
#if USE_DSI_DISPLAY
    int total_costumes = 0;
    for (auto &sprite : Scratch::sprites) total_costumes += (int)sprite->costumes.size();
    int costume_idx = 0;
    if (total_costumes > 0) loading_progress(tr(STR_LOADING_COSTUMES), 0, total_costumes);
#endif
    for (auto &sprite : Scratch::sprites) {
        for (auto &costume : sprite->costumes) {
            if (costume.fullName.empty()) continue;
            resolve_asset(costume.fullName, [&](const uint8_t *data, size_t len, bool) {
                renderer->loadCostumeFromMemory(costume.fullName,
                                                 data, len,
                                                 costume.rotationCenterX,
                                                 costume.rotationCenterY);
                int cw, ch;
                if (renderer->getCostumeSize(costume.fullName, cw, ch)) {
                    sprite->spriteWidth = cw;
                    sprite->spriteHeight = ch;
                }
                int tx, ty, tw, th;
                if (renderer->getCostumeTrimBounds(costume.fullName, tx, ty, tw, th)) {
                    costume.trimOffsetX = tx;
                    costume.trimOffsetY = ty;
                    costume.trimWidth = tw;
                    costume.trimHeight = th;
                }
            });
            // Yield CPU/PSRAM bandwidth between each costume rasterize.
            vTaskDelay(1);
#if USE_DSI_DISPLAY
            costume_idx++;
            loading_progress(tr(STR_LOADING_COSTUMES), costume_idx, total_costumes);
#endif
        }
    }

    usb_ll_write("INIT\n");
#if USE_DSI_DISPLAY
    // Loading is done. Tear down the blue backdrop + modal card so the brief
    // gap between the last asset loaded and the runtime task's first frame
    // shows plain black instead of a stale "loading" card on Scratch-blue.
    loading_end();
    dsi_clear_both_fbs(dsi_panel);
#endif
    // Force scratch_core's "turbo" path so scripts step every runtime-loop
    // iteration. The actual tick rate is set by the loop throttle below using
    // Scratch::FPS (parser reads the project's framerate, defaults to 30).
    // This is unrelated to Scratch's user-facing turbo mode (which means
    // "no yields between repeat iterations within a tick" — not implemented).
    // Without this override, FreeRTOS' 10ms tick granularity rounds the
    // throttle below the parser's 33ms checkFPS threshold and scripts skip
    // every other tick, running games at half speed.
    Scratch::turbo = true;
    // Pixel-perfect collision: bitmask is built lazily from SWRenderer RGBA
    // (see render_get_costume_rgba). Required for projects with sprites whose
    // SVG bounding box is much larger than the visible art (e.g., ground sprites
    // with extra decorative elements).
    Scratch::accurateCollision = true;
    Scratch::initializeScratchProject();

    // Auto-map gamepad buttons based on keys used in project
    auto_map_gamepad();

    if (!s_runtime_done) s_runtime_done = xSemaphoreCreateBinary();
    scratch_running = true;
    TaskHandle_t rt_handle = nullptr;
    // Allocate the 64KB stack in PSRAM — internal RAM gets fragmented after the first run.
    BaseType_t rt_ret = xTaskCreatePinnedToCoreWithCaps(
        scratch_runtime_task, "scratch_rt", 65536, nullptr, 5, &rt_handle, 1,
        MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "scratch_rt create: ret=%d handle=%p int=%u/%u psram=%u/%u",
             (int)rt_ret, rt_handle,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if (rt_ret != pdPASS) {
        scratch_running = false;
        return false;
    }

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
    loading_show(tr(STR_FETCHING_INFO), project_id);

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
    g_project_title.clear();
    if (api_json.contains("title") && api_json["title"].is_string()) {
        g_project_title = api_json["title"].get<std::string>();
    }

    // Step 2: Download project.json
    loading_show(tr(STR_LOADING_JSON), project_id);
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
    usb_ll_write("JSON_OK\n");

    // Persist project.json + create game directory so we can stream-write each
    // asset directly to SD (large projects don't fit in PSRAM). Drop any
    // PSRAM assets left over from a previous run *without* touching the JSON
    // we just parsed.
    for (auto &a : g_assets) {
        if (a.data) heap_caps_free(a.data);
    }
    g_assets.clear();
    g_assets_sd_project_id.clear();
    {
        const char *display_name = !g_project_title.empty() ? g_project_title.c_str() : project_id;
        if (sd_game_begin(project_id, display_name) &&
            sd_game_save_json(project_id, (const char *)json_data, json_len)) {
            g_assets_sd_project_id = project_id;
        } else {
            ESP_LOGW(TAG, "SD streaming setup failed; falling back to PSRAM-resident");
        }
    }
    heap_caps_free(json_data);

    {
        char hmsg[160];
        snprintf(hmsg, sizeof(hmsg),
            "HEAP after JSON: internal=%u (largest=%u) psram=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        usb_ll_write(hmsg);
    }

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

    // Sequential download in the calling task using a persistent keep-alive
    // WifiHttpSession. Avoids spawning helper tasks entirely.
    //
    // Why no parallel worker tasks: every multi-worker variant tripped
    // FreeRTOS's "TLSP deletion callback at index 0 overwritten" panic at
    // task deletion. Root cause is in ESP-IDF: TLS slot 0 is shared by
    // pthread (data + cleanup callback via ...AndDelCallback) and lwIP
    // (data only via vTaskSetThreadLocalStoragePointer with no bounds
    // protection on the callback half), and the worker's per-task
    // semaphore lifecycle ends up corrupting the callback slot. The
    // assert is in port.c::vPortTLSPointersDelCb, fires on vTaskDelete.
    // Not running vTaskDelete at all sidesteps the whole mess.
    //
    // Keep-alive on a single connection still gives the dominant speedup
    // (one TLS handshake for the whole project instead of per asset).
    int downloaded = 0;
    int skipped = 0;
    WifiHttpSession *session = wifi_http_session_open(
        "https://assets.scratch.mit.edu/internalapi/asset/");

    {
        char hmsg[96];
        snprintf(hmsg, sizeof(hmsg), "DL: keep-alive %s, %d assets\n",
                 session ? "on" : "off (fallback)", total_assets);
        usb_ll_write(hmsg);
    }

    for (auto &name : asset_names) {
        loading_progress(tr(STR_ASSETS), downloaded, total_assets);

        snprintf(url, sizeof(url),
                 "https://assets.scratch.mit.edu/internalapi/asset/%s/get/",
                 name.c_str());

        size_t alen = 0;
        uint8_t *adata = nullptr;
        if (session) {
            adata = wifi_http_session_get(session, url, &alen);
        }
        if (!adata) {
            adata = wifi_http_get(url, &alen);
        }

        if (!adata) {
            skipped++;
            char msg[128];
            snprintf(msg, sizeof(msg), "WARN:asset skip %s\n", name.c_str());
            usb_ll_write(msg);
            continue;
        }

        // Stream straight to SD when streaming mode is active; otherwise
        // (SD setup failed) fall back to PSRAM accumulation.
        if (!g_assets_sd_project_id.empty()) {
            bool ok = sd_game_save_asset(g_assets_sd_project_id.c_str(),
                                         name.c_str(), adata, alen);
            heap_caps_free(adata);
            if (!ok) {
                skipped++;
                char msg[128];
                snprintf(msg, sizeof(msg), "WARN:sd-write fail %s\n", name.c_str());
                usb_ll_write(msg);
                continue;
            }
        } else {
            g_assets.push_back({name, adata, alen});
        }
        downloaded++;

        char msg[128];
        snprintf(msg, sizeof(msg), "ASSET %d/%d: %s (%zu B)\n",
                 downloaded, total_assets, name.c_str(), alen);
        usb_ll_write(msg);
    }

    if (session) wifi_http_session_close(session);

    char msg[80];
    snprintf(msg, sizeof(msg), "DL_DONE: %d/%d assets (skipped %d)\n",
             downloaded, total_assets, skipped);
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
    if (!copy) {
        ESP_LOGE(TAG, "sd_asset_load_cb: alloc FAILED for %s (%zu bytes), psram_free=%u largest=%u",
                 name, len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return;
    }
    memcpy(copy, data, len);
    g_assets.push_back({std::string(name), copy, len});
}

static bool load_game_from_sd(const char *project_id)
{
    free_assets();

    // Read JSON only — assets are pulled lazily from SD by the runtime
    // resolver (see g_assets_sd_project_id usage). Big projects can't fit
    // all assets in PSRAM at once.
    size_t json_len = 0;
    char *json_str = sd_read_project_json(project_id, &json_len);
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

    g_assets_sd_project_id = project_id;
    return true;
}

// ============================================================
// Save downloaded project to SD card
// ============================================================

static void save_current_project_to_sd(const char *project_id)
{
    // Streamed downloads have already written everything to SD during DL.
    if (!g_assets_sd_project_id.empty() && g_assets_sd_project_id == project_id) {
        ESP_LOGI(TAG, "save_to_sd: already streamed during DL, skipping");
        return;
    }
    if (!g_project_json || g_assets.empty()) {
        ESP_LOGW(TAG, "save_to_sd skipped: json=%p assets=%zu",
                 g_project_json, g_assets.size());
        return;
    }

    std::string json_str = g_project_json->dump();

    // Build SdAsset array
    std::vector<SdAsset> sd_assets(g_assets.size());
    for (size_t i = 0; i < g_assets.size(); i++) {
        sd_assets[i].name = g_assets[i].name.c_str();
        sd_assets[i].data = g_assets[i].data;
        sd_assets[i].len = g_assets[i].len;
    }

    const char *display_name = !g_project_title.empty() ? g_project_title.c_str() : project_id;
    sd_save_game(project_id, display_name,
                 json_str.c_str(), json_str.size(),
                 sd_assets.data(), (int)sd_assets.size());
}

// ============================================================
// In-game polling for Start/Select system buttons
// ============================================================

static void game_overlay_check()
{
    InputDev::State s = InputDev::get();

    // Start → exit confirm
    if (s.buttons & InputDev::START) {
        while (InputDev::get().buttons & InputDev::START) vTaskDelay(pdMS_TO_TICKS(20));
        SoundPlayer::playSound("ui_select");

        // Let the runtime finish the in-flight frame, then freeze display + audio
        // (mixer keeps draining DMA but writes silence — sounds resume on dismiss)
        scratch_paused = true;
        audio_mixer_set_suspended(true);
        vTaskDelay(pdMS_TO_TICKS(50));

        bool quit = ui_show_exit_confirm();
        scratch_paused = false;
        audio_mixer_set_suspended(false);
        if (quit) {
            scratch_running = false;
        }
    }

    // Back/Select → show button map
    if (s.buttons & InputDev::BACK) {
        while (InputDev::get().buttons & InputDev::BACK) vTaskDelay(pdMS_TO_TICKS(20));
        SoundPlayer::playSound("ui_select");
        scratch_paused = true;
        audio_mixer_set_suspended(true);
        vTaskDelay(pdMS_TO_TICKS(50));
        ui_show_button_map();
        scratch_paused = false;
        audio_mixer_set_suspended(false);
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

    // Asset reader the renderer uses to refill an evicted costume's RGBA.
    // Looks up g_assets first (USB-fed projects keep raw bytes resident);
    // otherwise falls back to /sd/games/<id>/assets/<name>.
    renderer->setAssetReader([](const std::string &name) -> std::vector<uint8_t> {
        // The vector copy needs a contiguous PSRAM block the size of the
        // asset (sometimes a few MB for big PNGs). When PSRAM is fragmented,
        // the std::vector ctor throws bad_alloc; without a catch it
        // propagates up to terminate(). Swallow the throw so the renderer
        // just sees "decode failed" and the sprite stays as it was.
        try {
            for (auto &asset : g_assets) {
                if (asset.name == name) {
                    return std::vector<uint8_t>(asset.data, asset.data + asset.len);
                }
            }
            if (!g_assets_sd_project_id.empty()) {
                size_t alen = 0;
                uint8_t *adata = sd_read_asset(g_assets_sd_project_id.c_str(),
                                                name.c_str(), &alen);
                if (adata) {
                    std::vector<uint8_t> v(adata, adata + alen);
                    heap_caps_free(adata);
                    return v;
                }
            }
        } catch (const std::bad_alloc &) {
            ESP_LOGW(TAG, "asset reader: bad_alloc for %s (PSRAM fragmented)",
                     name.c_str());
        }
        return {};
    });

#if USE_DSI_DISPLAY
    dsi_panel = dsi_display_init();
    es8388_audio_init();
    battery_monitor_init(g_i2c_bus);
#else
    lcd_fb = (uint16_t *)heap_caps_malloc(LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
    lcd_panel = lcd_init();
#endif

    vTaskDelay(pdMS_TO_TICKS(500));
    input_xbox_init();
    input_gpio_init();
    touch_input_init();
    sd_init();

    // Load saved UI language (depends on SD being mounted; defaults to EN)
    lang_init();

#if USE_DSI_DISPLAY
    // Initialize LVGL menu system
    ui_init(dsi_panel);

    // Boot splash: ScratchESP logo + spinner. Held briefly so it's visible
    // even when WiFi auto-connect kicks in immediately.
    ui_show_splash();
    vTaskDelay(pdMS_TO_TICKS(1200));

    // Boot-time WiFi connection (try saved credentials)
    {
        wifi_init();
        char saved_ssid[64] = {}, saved_pass[128] = {};
        if (sd_load_wifi(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
            ui_show_wifi_connecting(saved_ssid);
            bool ok = wifi_connect(saved_ssid, saved_pass, 10000);
            ui_show_wifi_result(ok, saved_ssid);
            if (ok) time_sync_start();
            vTaskDelay(pdMS_TO_TICKS(ok ? 1000 : 2000));
        }
    }

    // Main loop: menu → game → menu → ...
    while (true) {
        MenuAction action = ui_menu_run();

        if (action == MenuAction::PLAY_FROM_SD) {
            const char *pid = ui_get_selected_project_id();

            // WiFi SDIO RX path keeps allocating PSRAM buffers; once the
            // game starts and PSRAM fills up with sounds/costumes the SDIO
            // task asserts in copy_payload (sdio_drv.c:977). Disconnect now
            // — the game runs offline. (QR path already disconnects after
            // the download completes.)
            if (wifi_is_connected()) {
                ESP_LOGI(TAG, "PLAY_FROM_SD: wifi_disconnect");
                wifi_disconnect();
            }

            // Hand the screen to the unified DSI loading modal: suspend LVGL,
            // wipe the FBs, and from here every status update goes through
            // loading_show/loading_progress so the user sees one continuous
            // card instead of LVGL flashing into a black screen and back.
            ui_suspend();
            dsi_clear_both_fbs(dsi_panel);
            loading_begin();
            loading_show(tr(STR_LOADING), pid);

            if (!load_game_from_sd(pid)) {
                loading_show(tr(STR_LOAD_FAILED), pid);
                vTaskDelay(pdMS_TO_TICKS(2000));
                loading_end();
                ui_resume();
                continue;
            }
        } else if (action == MenuAction::PLAY_FROM_QR) {
            const char *pid = ui_get_selected_project_id();

            ui_suspend();
            dsi_clear_both_fbs(dsi_panel);
            loading_begin();

            // Ensure WiFi is connected before download
            if (!wifi_is_connected()) {
                char ssid[64] = {}, pass[128] = {};
                if (sd_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
                    loading_show(tr(STR_WIFI_CONNECTING), ssid);
                    if (!wifi_connect(ssid, pass, 10000)) {
                        loading_show(tr(STR_WIFI_FAILED), ssid);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        loading_end();
                        ui_resume();
                        continue;
                    }
                    time_sync_start();
                } else {
                    loading_show(tr(STR_NO_WIFI), tr(STR_NO_WIFI_DETAIL));
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    loading_end();
                    ui_resume();
                    continue;
                }
            }

            ESP_LOGI(TAG, "DL: calling qr_download_project");
            loading_show(tr(STR_DOWNLOADING), pid);

            if (!qr_download_project(pid)) {
                loading_show(tr(STR_DL_FAILED), pid);
                vTaskDelay(pdMS_TO_TICKS(2000));
                wifi_disconnect();
                loading_end();
                ui_resume();
                continue;
            }

            ESP_LOGI(TAG, "DL: download complete");

            ESP_LOGI(TAG, "DL: wifi_disconnect");
            wifi_disconnect();

            ESP_LOGI(TAG, "DL: save_to_sd");
            save_current_project_to_sd(pid);
            ESP_LOGI(TAG, "DL: save_to_sd done");
        } else {
            continue;
        }

        // Modal stays on screen through load_and_run; the runtime task's
        // first frame overwrites it once sprites are fully loaded.
        if (load_and_run()) {
            // Runtime task is now drawing; the loading-modal solid backdrop
            // is no longer on screen, so future modal calls (e.g. the in-game
            // exit confirm overlay) should resume their default dim behavior.
            loading_end();
            // Game loop: check for Start/Select overlays
            while (scratch_running) {
                game_overlay_check();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        } else {
            loading_end();
        }

        // Wait for runtime task to fully exit (frees its 64KB stack)
        if (s_runtime_done) {
            xSemaphoreTake(s_runtime_done, pdMS_TO_TICKS(2000));
        }
        // Give the idle task a few ticks to actually reap the deleted TCB+stack
        vTaskDelay(pdMS_TO_TICKS(100));

        // Stop and free all sounds before returning to menu — otherwise the
        // mixer keeps playing whatever was active when the game exited and the
        // BGM bleeds into the menu UI.
        SoundPlayer::cleanupAudio();
        audio_mixer_set_suspended(false);  // ensure resume state for next game

        // Tear down all scratch_core runtime state: deletes every Sprite*
        // (otherwise sprites.clear() at next load_and_run leaks them along
        // with their blocks/costumes/sounds/variables), drops costumeImages
        // cache, broadcast/clone/backdrop queues, stageSprite, answer,
        // customUsername, monitors, SpeechManager, TextObject cache,
        // DownloadManager. Also calls Render::penClear() which routes
        // through our pen callback to wipe the pen layer.
        Scratch::cleanupScratchProject();

        // Reset transient input state so the next game doesn't see keys
        // "still pressed" from the one that just exited.
        Input::keyHeldDuration.clear();
        Input::inputBuffer.clear();
        Input::codePressedBlockOpcodes.clear();

        // Free rasterized costume RGBAs (~10 MB for Ninja Demo). Otherwise
        // PSRAM stays fragmented and the next project's 4 MB BGM allocation
        // fails inside sd_asset_load_cb, falling back to a 10 s CDN download.
        if (renderer) renderer->clearCostumes();

        // Game ended — clean up and return to menu
        free_assets();
        // ui_resume() happens at top of next ui_menu_run() iteration.
        // WiFi stays disconnected until the user picks a new QR project; the
        // system clock keeps ticking from the boot-time SNTP sync.
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
