// LVGL-based menu system for Scratcher
// Renders to DSI DPI framebuffer (720x1280 portrait) with 270 rotation for landscape

#include "ui_menu.h"
#include "dsi_display.h"
#include "dsi_modal.h"
#include "input_device.h"
#include "touch_input.h"
#include "sd_storage.h"
#include "wifi_manager.h"
#include "camera_qr.h"
#include "es8388_audio.h"
#include "i18n.h"
#include "battery_monitor.h"
#include "time_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/ppa.h"

#include "lvgl.h"
#include "src/libs/tiny_ttf/lv_tiny_ttf.h"

#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>

// scratch_core: read the project's gamepad→key auto-mapping table.
#include <input.hpp>
#include <audio.hpp>

// Embedded NotoSansJP TTF (linked via EMBED_FILES in scratch_core)
extern "C" const uint8_t noto_sans_ttf_start[] asm("_binary_NotoSansJP_Medium_subset_ttf_start");
extern "C" const uint8_t noto_sans_ttf_end[]   asm("_binary_NotoSansJP_Medium_subset_ttf_end");

// Pre-converted UI assets (RGB565A8 dsc declared in assets_gen/ui_*.c)
extern "C" const lv_image_dsc_t ui_logo;
extern "C" const lv_image_dsc_t ui_icon_play;
extern "C" const lv_image_dsc_t ui_icon_new;
extern "C" const lv_image_dsc_t ui_icon_settings;

// Embedded UI sound effects (16bit mono 48kHz WAV via EMBED_FILES)
extern "C" const uint8_t cursor_wav_start[] asm("_binary_cursor_wav_start");
extern "C" const uint8_t cursor_wav_end[]   asm("_binary_cursor_wav_end");
extern "C" const uint8_t select_wav_start[] asm("_binary_select_wav_start");
extern "C" const uint8_t select_wav_end[]   asm("_binary_select_wav_end");
extern "C" const uint8_t cancel_wav_start[] asm("_binary_cancel_wav_start");
extern "C" const uint8_t cancel_wav_end[]   asm("_binary_cancel_wav_end");

static void load_ui_sounds() {
    SoundPlayer::loadSoundFromMemory("ui_cursor", cursor_wav_start,
                                     cursor_wav_end - cursor_wav_start);
    SoundPlayer::loadSoundFromMemory("ui_select", select_wav_start,
                                     select_wav_end - select_wav_start);
    SoundPlayer::loadSoundFromMemory("ui_cancel", cancel_wav_start,
                                     cancel_wav_end - cancel_wav_start);
}

// True between when the user presses a nav direction and when LVGL processes
// the resulting focus change. Used so the focus_cb only chirps on real
// user-driven moves — programmatic focus during screen build stays silent.
static volatile bool s_user_nav_pending = false;

static void ui_group_focus_cb(lv_group_t * /*group*/) {
    // s_user_nav_pending is set strictly by indev_read_cb whenever a nav
    // direction is held this poll, so we don't clear it here — auto-repeat
    // navigation needs the flag to stay true across consecutive focus moves.
    if (s_user_nav_pending) {
        SoundPlayer::playSound("ui_cursor");
    }
}

// Pick the closest pre-baked Montserrat font for a desired pixel size.
// Used as a fallback for the JP TTF (NotoSansJP subset doesn't include LV_SYMBOL_*
// FontAwesome glyphs) and as the primary font in EN mode.
static const lv_font_t *montserrat_for(int px) {
    if (px <= 14) return &lv_font_montserrat_14;
    if (px <= 16) return &lv_font_montserrat_16;
    if (px <= 18) return &lv_font_montserrat_18;
    if (px <= 20) return &lv_font_montserrat_20;
    if (px <= 24) return &lv_font_montserrat_24;
    return &lv_font_montserrat_36;
}

// Cache up to a handful of JP font sizes so multiple labels can coexist.
struct JpFontEntry { int px; lv_font_t *font; };
static JpFontEntry s_jp_cache[8] = {};

static lv_font_t *get_jp_font(int px) {
    for (auto &e : s_jp_cache) {
        if (e.font && e.px == px) return e.font;
    }
    size_t len = (size_t)(noto_sans_ttf_end - noto_sans_ttf_start);
    lv_font_t *f = lv_tiny_ttf_create_data(noto_sans_ttf_start, len, px);
    if (!f) return nullptr;
    // Chain Montserrat as fallback for missing glyphs (LV_SYMBOL_*, etc).
    f->fallback = montserrat_for(px);
    for (auto &e : s_jp_cache) {
        if (!e.font) { e.px = px; e.font = f; return f; }
    }
    // Cache full — return the font but it leaks if we keep recreating.
    return f;
}

// Wrap a Montserrat font with a NotoSansJP fallback, so EN-mode labels can still
// render Japanese characters (e.g. game titles, WiFi SSIDs). lv_font_montserrat_*
// lives in flash and is const, so we copy the struct into a writable cache and
// patch only the fallback pointer.
struct MontFbEntry { int px; const lv_font_t *base; lv_font_t copy; bool inited; };
static MontFbEntry s_mont_fb[] = {
    {14, &lv_font_montserrat_14, {}, false},
    {16, &lv_font_montserrat_16, {}, false},
    {18, &lv_font_montserrat_18, {}, false},
    {20, &lv_font_montserrat_20, {}, false},
    {24, &lv_font_montserrat_24, {}, false},
    {36, &lv_font_montserrat_36, {}, false},
};

static const lv_font_t *montserrat_with_jp_fb(int px) {
    MontFbEntry *picked = &s_mont_fb[0];
    for (auto &m : s_mont_fb) {
        picked = &m;
        if (px <= m.px) break;
    }
    if (!picked->inited) {
        picked->copy = *picked->base;
        picked->copy.fallback = get_jp_font(picked->px);
        picked->inited = true;
    }
    return &picked->copy;
}

// Pick the menu font for a desired pixel size based on the current language.
// EN: Montserrat → NotoSansJP fallback (so JP user-content like game titles renders).
// JP: NotoSansJP → Montserrat fallback (so LV_SYMBOL FontAwesome glyphs render).
static const lv_font_t *menu_font(int px) {
    if (lang_get() == Lang::JP) {
        lv_font_t *f = get_jp_font(px);
        if (f) return f;
        return montserrat_for(px);
    }
    return montserrat_with_jp_fb(px);
}

static const char *TAG = "ui_menu";

// ============================================================
// LVGL display + input state
// ============================================================

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t *s_disp = nullptr;
static lv_indev_t *s_indev = nullptr;
static lv_indev_t *s_indev_touch = nullptr;
static lv_group_t *s_group = nullptr;
static TaskHandle_t s_lvgl_task = nullptr;
static SemaphoreHandle_t s_lvgl_mutex = nullptr;

// (input state read directly in indev callback)

// When true, LVGL flush is a no-op (camera/game owns DPI FB)
static volatile bool s_flush_disabled = false;

// Menu state
static MenuState s_state = MenuState::MAIN_MENU;
static MenuAction s_action = MenuAction::NONE;
static char s_selected_project_id[64] = {};
// True when QR_WIFI was entered from the settings card; the QR flow returns
// to settings instead of the main menu when this is set.
static bool s_qr_wifi_from_settings = false;
// Last focused main-menu tile index (0=Games, 1=NewGame, 2=Settings); restored
// when returning to the main menu so focus doesn't snap back to "あそぶ".
static int s_main_menu_focus_idx = 0;

// Settings
// Brightness/volume are stored as 10%-step counts (1..10 for brightness,
// 0..10 for volume). The on-screen label shows step*10 + "%". This keeps the
// slider chunky enough that one d-pad press = 10%, which is comfortable for
// kids while still letting touch drag pick any level.
static int s_brightness_step = 10;   // 1..10  (=10%..100%)
static int s_volume_step = 8;        // 0..10  (0=mute, =0%..100%)
static bool s_muted = false;         // legacy; reflects (s_volume_step == 0)

#define BRIGHTNESS_MAX_STEP 10
#define VOLUME_MAX_STEP     10
#define STEP_TO_PCT(s)      ((s) * 10)

// ============================================================
// LVGL draw buffer (landscape 1280x720 RGB565 in PSRAM)
// ============================================================

static uint8_t *s_lvgl_buf = nullptr;
#define LVGL_BUF_W  DSI_LANDSCAPE_W   // 1280
#define LVGL_BUF_H  DSI_LANDSCAPE_H   // 720
#define LVGL_BUF_SIZE (LVGL_BUF_W * LVGL_BUF_H * 2)

// PPA client for LVGL flush rotation
static ppa_client_handle_t s_lvgl_ppa = nullptr;
static SemaphoreHandle_t s_lvgl_ppa_done = nullptr;

static bool lvgl_ppa_done_cb(ppa_client_handle_t client, ppa_event_data_t *event_data, void *user_data) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_lvgl_ppa_done, &woken);
    return woken == pdTRUE;
}

// ============================================================
// LVGL flush callback — PPA rotate landscape→portrait DPI FB
// ============================================================

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ESP_LOGW("BLUE_DBG", "lvgl_flush_cb disabled=%d area=(%d,%d)-(%d,%d)",
             (int)s_flush_disabled, (int)area->x1, (int)area->y1,
             (int)area->x2, (int)area->y2);
    // Skip flush when camera or game owns the DPI framebuffer
    if (s_flush_disabled) {
        lv_display_flush_ready(disp);
        return;
    }

    // FULL mode: px_map is the complete 1280x720 landscape buffer.
    // PPA SRM rotate 270° → 720x1280 portrait DPI back buffer.
    void *dpi_fb = dsi_get_back_fb();
    if (!dpi_fb) {
        lv_display_flush_ready(disp);
        return;
    }

    // Sync LVGL buffer from CPU cache to PSRAM
    esp_cache_msync(px_map, LVGL_BUF_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // Wait for previous PPA op
    xSemaphoreTake(s_lvgl_ppa_done, pdMS_TO_TICKS(100));

    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = px_map;
    srm.in.pic_w = LVGL_BUF_W;
    srm.in.pic_h = LVGL_BUF_H;
    srm.in.block_w = LVGL_BUF_W;
    srm.in.block_h = LVGL_BUF_H;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    srm.out.buffer = dpi_fb;
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
    srm.rgb_swap = false;
    srm.byte_swap = false;
    srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    srm.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(s_lvgl_ppa, &srm);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LVGL PPA SRM failed: %s", esp_err_to_name(ret));
    }
    xSemaphoreGive(s_lvgl_ppa_done);

    // Submit the back buffer for display, swap.
    dsi_present(s_panel);

    lv_display_flush_ready(disp);
}

// ============================================================
// LVGL pointer read callback — touch panel mapped into landscape (1280x720),
// the same orientation LVGL is rendering at.
// ============================================================

static void lvgl_pointer_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_raw_t t;
    if (!touch_input_get_raw(&t)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Panel(720x1280 portrait) → landscape(1280x720). DPI panel is rotated
    // 270° in dsi_display.cpp, so:
    //   landscape_x = (PANEL_H - 1) - panel_y
    //   landscape_y = panel_x
    int lx = (1280 - 1) - t.y;
    int ly = t.x;
    if (lx < 0) lx = 0;
    if (lx > 1279) lx = 1279;
    if (ly < 0) ly = 0;
    if (ly > 719) ly = 719;

    data->point.x = lx;
    data->point.y = ly;
    data->state = t.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// ============================================================
// LVGL input read callback — InputDev to keypad
// ============================================================

static int s_dbg_cnt = 0;

static void lvgl_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    InputDev::State gs = InputDev::get();
    uint32_t b = gs.buttons;

    // Debug: log every 60th call (~1/sec) and whenever buttons pressed
    s_dbg_cnt++;
    if (b != 0 || (s_dbg_cnt % 60 == 0)) {
        ESP_LOGI("indev", "btn=0x%08lX conn=%d grp_cnt=%u",
                 (unsigned long)b, gs.any_connected,
                 s_group ? (unsigned)lv_group_get_obj_count(s_group) : 0);
    }

    // Report current button state — LVGL handles repeat/debounce internally
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;

    // D-pad UP/DOWN → navigate. LEFT/RIGHT → adjust in edit mode (sliders),
    // otherwise also navigate (so horizontal Yes/No is reachable with the d-pad).
    // Special case: when a dropdown is open, UP/DOWN must scroll its options
    // (LV_KEY_UP/DOWN) instead of jumping to the previous/next group widget.
    bool editing = s_group && lv_group_get_editing(s_group);
    bool dropdown_open = false;
    bool slider_focused = false;
    if (s_group) {
        lv_obj_t *focused = lv_group_get_focused(s_group);
        if (focused && lv_obj_check_type(focused, &lv_dropdown_class)) {
            dropdown_open = lv_dropdown_is_open(focused);
        }
        if (focused && lv_obj_check_type(focused, &lv_slider_class)) {
            slider_focused = true;
        }
    }
    // Slider focused → LEFT/RIGHT directly adjusts value (no edit-mode needed)
    bool horiz_adjusts = slider_focused || editing;

    // Detect whether the focused widget lives in a horizontal or vertical
    // layout, so UP/DOWN don't move sideways on a horizontal row of tiles
    // (and vice versa). Walk up from the focused widget to the closest
    // ancestor that contains ≥2 group members and has a flex flow set.
    enum { AXIS_NONE, AXIS_HOR, AXIS_VER } axis = AXIS_NONE;
    if (s_group) {
        lv_obj_t *focused = lv_group_get_focused(s_group);
        uint32_t gcount = lv_group_get_obj_count(s_group);
        for (lv_obj_t *anc = focused ? lv_obj_get_parent(focused) : nullptr;
             anc != nullptr; anc = lv_obj_get_parent(anc)) {
            uint32_t members = 0;
            for (uint32_t i = 0; i < gcount && members < 2; i++) {
                lv_obj_t *o = lv_group_get_obj_by_index(s_group, i);
                for (lv_obj_t *p = o; p != nullptr; p = lv_obj_get_parent(p)) {
                    if (p == anc) { members++; break; }
                }
            }
            if (members < 2) continue;
            lv_flex_flow_t flow = (lv_flex_flow_t)lv_obj_get_style_flex_flow(anc, 0);
            // Bit 0 (LV_FLEX_COLUMN) distinguishes column from row. If flex
            // isn't configured on this ancestor at all, leave axis = NONE.
            if (lv_obj_get_style_layout(anc, 0) == LV_LAYOUT_FLEX) {
                axis = (flow & LV_FLEX_COLUMN) ? AXIS_VER : AXIS_HOR;
            }
            break;
        }
    }
    // Suppress the "wrong axis" d-pad keys so a horizontal row doesn't move
    // on UP/DOWN, and a vertical column doesn't move on LEFT/RIGHT.
    bool suppress_vert = (axis == AXIS_HOR);
    bool suppress_horiz_nav = (axis == AXIS_VER);

    if ((b & InputDev::DPAD_UP) && (dropdown_open || !suppress_vert)) {
        data->key = dropdown_open ? LV_KEY_UP : LV_KEY_PREV;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if ((b & InputDev::DPAD_DOWN) && (dropdown_open || !suppress_vert)) {
        data->key = dropdown_open ? LV_KEY_DOWN : LV_KEY_NEXT;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if ((b & InputDev::DPAD_LEFT) && (horiz_adjusts || !suppress_horiz_nav)) {
        data->key = horiz_adjusts ? LV_KEY_LEFT : LV_KEY_PREV;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if ((b & InputDev::DPAD_RIGHT) && (horiz_adjusts || !suppress_horiz_nav)) {
        data->key = horiz_adjusts ? LV_KEY_RIGHT : LV_KEY_NEXT;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (b & InputDev::A) {
        data->key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    // B is handled by the menu loop only (closes dropdown OR pops screen).
    // KEYPAD indev doesn't use editing mode (LVGL forcibly disables it in
    // lv_indev_keypad_proc), so we don't need ESC for edit-mode exit.
    // Sending ESC here would race the menu loop and either close the
    // dropdown twice or let the back-nav fire wrongly.

    // Left stick fallback (with deadzone)
    if (data->key == 0) {
        const int16_t DZ = 16000;
        if (gs.ly > DZ && (dropdown_open || !suppress_vert)) {
            data->key = dropdown_open ? LV_KEY_UP : LV_KEY_PREV;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (gs.ly < -DZ && (dropdown_open || !suppress_vert)) {
            data->key = dropdown_open ? LV_KEY_DOWN : LV_KEY_NEXT;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (gs.lx < -DZ && (horiz_adjusts || !suppress_horiz_nav)) {
            data->key = horiz_adjusts ? LV_KEY_LEFT : LV_KEY_PREV;
            data->state = LV_INDEV_STATE_PRESSED;
        } else if (gs.lx > DZ && (horiz_adjusts || !suppress_horiz_nav)) {
            data->key = horiz_adjusts ? LV_KEY_RIGHT : LV_KEY_NEXT;
            data->state = LV_INDEV_STATE_PRESSED;
        }
    }

    // UI sound triggers — only when LVGL UI is actually on screen.
    // - Cursor: handled separately via lv_group_set_focus_cb (s_user_nav_pending
    //   flag distinguishes user-driven focus changes from programmatic ones).
    // - Select: rising edge of A. Almost always activates something.
    // - Cancel: explicit calls at the actual "go back" code paths, not here —
    //   pressing B on the main menu (no back target) shouldn't make a sound.
    static uint32_t s_prev_btn = 0;
    uint32_t edge = b & ~s_prev_btn;
    if (!s_flush_disabled) {
        constexpr uint32_t NAV_MASK = InputDev::DPAD_UP | InputDev::DPAD_DOWN
                                    | InputDev::DPAD_LEFT | InputDev::DPAD_RIGHT;
        bool nav_held = (b & NAV_MASK) != 0
                     || std::abs(gs.lx) > 16000 || std::abs(gs.ly) > 16000;
        // Strict: only true while a nav direction is actually held *this poll*.
        // Programmatic focus changes (screen build, lv_group_focus_obj) happen
        // when nothing is held → focus_cb sees flag=false → stays silent.
        s_user_nav_pending = nav_held;
        // Select only when something focusable is actually receiving the press —
        // loading/status screens have no focused widget, so they stay silent.
        if ((edge & InputDev::A) && s_group && lv_group_get_focused(s_group)) {
            SoundPlayer::playSound("ui_select");
        }
    } else {
        s_user_nav_pending = false;
    }
    s_prev_btn = b;
}

// ============================================================
// LVGL tick (called from timer or task)
// ============================================================

static void lvgl_task_fn(void *param)
{
    while (true) {
        if (xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lvgl_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(16)); // ~60fps tick rate
    }
}

static uint32_t lvgl_tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ============================================================
// Screen builders
// ============================================================

static lv_obj_t *s_scr_main = nullptr;
static lv_obj_t *s_scr_games = nullptr;
static lv_obj_t *s_scr_settings = nullptr;

// Forward declarations
static void build_main_menu();
static void build_game_list();
static void build_settings();
static void show_screen(lv_obj_t *scr);

// ============================================================
// Style definitions
// ============================================================

static lv_style_t s_style_bg;
static lv_style_t s_style_btn;
static lv_style_t s_style_btn_focus;
static lv_style_t s_style_title;
static lv_style_t s_style_widget_focus;  // For sliders, switches, etc.
static bool s_styles_inited = false;

// Scratch 3.0 brand colors
#define SCRATCH_ORANGE     0xFF8C1A
#define SCRATCH_ORANGE_LT  0xFFAB19
#define SCRATCH_YELLOW     0xFFBF00
#define SCRATCH_BLUE       0x4C97FF
#define SCRATCH_BLUE_DARK  0x3373CC
#define SCRATCH_WHITE      0xFFFFFF
#define SCRATCH_BG         0x4C97FF  // Scratch blue background
#define SCRATCH_CARD       0xFFFFFF  // White cards
#define SCRATCH_TEXT       0x575E75  // Scratch dark gray text

static void init_styles()
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    // Scratch blue background
    lv_style_init(&s_style_bg);
    lv_style_set_bg_color(&s_style_bg, lv_color_hex(SCRATCH_BG));
    lv_style_set_bg_opa(&s_style_bg, LV_OPA_COVER);
    lv_style_set_text_color(&s_style_bg, lv_color_hex(SCRATCH_WHITE));

    // Button normal — white rounded card
    lv_style_init(&s_style_btn);
    lv_style_set_bg_color(&s_style_btn, lv_color_hex(SCRATCH_CARD));
    lv_style_set_bg_opa(&s_style_btn, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_btn, lv_color_hex(0xD9D9D9));
    lv_style_set_border_width(&s_style_btn, 2);
    lv_style_set_radius(&s_style_btn, 16);
    lv_style_set_pad_all(&s_style_btn, 16);
    lv_style_set_text_color(&s_style_btn, lv_color_hex(SCRATCH_TEXT));
    lv_style_set_shadow_color(&s_style_btn, lv_color_hex(0x00000040));
    lv_style_set_shadow_width(&s_style_btn, 8);
    lv_style_set_shadow_ofs_y(&s_style_btn, 3);

    // Button focused — Scratch orange, very visible
    lv_style_init(&s_style_btn_focus);
    lv_style_set_bg_color(&s_style_btn_focus, lv_color_hex(SCRATCH_ORANGE));
    lv_style_set_border_color(&s_style_btn_focus, lv_color_hex(SCRATCH_YELLOW));
    lv_style_set_border_width(&s_style_btn_focus, 4);
    lv_style_set_text_color(&s_style_btn_focus, lv_color_hex(SCRATCH_WHITE));
    lv_style_set_shadow_color(&s_style_btn_focus, lv_color_hex(SCRATCH_ORANGE_LT));
    lv_style_set_shadow_width(&s_style_btn_focus, 12);
    lv_style_set_shadow_spread(&s_style_btn_focus, 0);

    // Title — white on blue
    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, lv_color_hex(SCRATCH_WHITE));

    // Widget focus — orange outline for sliders, switches, etc.
    lv_style_init(&s_style_widget_focus);
    lv_style_set_outline_color(&s_style_widget_focus, lv_color_hex(SCRATCH_ORANGE));
    lv_style_set_outline_width(&s_style_widget_focus, 4);
    lv_style_set_outline_pad(&s_style_widget_focus, 6);
    lv_style_set_outline_opa(&s_style_widget_focus, LV_OPA_COVER);
    lv_style_set_border_color(&s_style_widget_focus, lv_color_hex(SCRATCH_ORANGE));
    lv_style_set_border_width(&s_style_widget_focus, 3);

    // Synchronized focus transition: bg / border / shadow all animate together.
    // Without this, LVGL's default theme animates bg_color (~80ms) but applies
    // border/shadow instantly, causing the frame to "lead" the fill on focus.
    static const lv_style_prop_t focus_trans_props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_BORDER_WIDTH,
        LV_STYLE_BORDER_OPA,
        LV_STYLE_SHADOW_COLOR,
        LV_STYLE_SHADOW_WIDTH,
        LV_STYLE_SHADOW_SPREAD,
        LV_STYLE_SHADOW_OPA,
        LV_STYLE_TEXT_COLOR,
        LV_STYLE_PROP_INV,
    };
    static lv_style_transition_dsc_t focus_trans;
    lv_style_transition_dsc_init(&focus_trans, focus_trans_props,
                                 lv_anim_path_linear, 30, 0, NULL);
    lv_style_set_transition(&s_style_btn,       &focus_trans);
    lv_style_set_transition(&s_style_btn_focus, &focus_trans);
}

// Helper: create a styled button with label
static lv_obj_t *create_menu_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data = nullptr)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_style(btn, &s_style_btn, 0);
    lv_obj_add_style(btn, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(btn, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_width(btn, lv_pct(80));
    lv_obj_set_height(btn, 64);
    lv_obj_set_style_text_font(btn, menu_font(20), 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);

    lv_group_add_obj(s_group, btn);
    return btn;
}

// Helper: create a tall tile button with icon on top and label below.
// Used for the main menu's 3 horizontal tiles.
// Icon dsc is 320x320 source; LVGL scales it down via image zoom.
static lv_obj_t *create_tile_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                 const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_add_style(btn, &s_style_btn, 0);
    lv_obj_add_style(btn, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(btn, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, 240, 280);
    lv_obj_set_style_pad_all(btn, 16, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 8, 0);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    // Icon: 160x160 source displayed at native size (no runtime scaling)
    lv_obj_t *img = lv_image_create(btn);
    lv_image_set_src(img, icon);
    lv_obj_set_size(img, 160, 160);
    lv_obj_set_style_pad_all(img, 0, 0);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    // Label below the icon (wraps to next line if too long)
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, menu_font(22), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 208);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    lv_group_add_obj(s_group, btn);
    return btn;
}

// ============================================================
// Settings card — full-width white rounded row with a left label and a
// right-side content area. Used for the stacked settings layout.
// Returns the inner content slot (right side); caller adds widgets there.
// ============================================================

static lv_obj_t *create_settings_card(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 110);
    lv_obj_set_style_bg_color(card, lv_color_hex(SCRATCH_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x00000040), 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_ofs_y(card, 3, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 16, 0);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, menu_font(22), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(SCRATCH_TEXT), 0);
    lv_obj_set_width(label, 220);

    // Content slot — fills the remaining width
    lv_obj_t *content = lv_obj_create(card);
    lv_obj_remove_style_all(content);
    lv_obj_set_height(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(content, 16, 0);
    // Inset children by 10px horizontally so the slider/dropdown's focused
    // orange outline (pad 6 + width 4 = 10px outside the bounding box) fits
    // within content's clip rect — otherwise the left side gets clipped.
    lv_obj_set_style_pad_hor(content, 10, 0);
    return content;
}

// ============================================================
// Chrome bars (top: clock + battery, bottom: control hints)
// Switch-style — added to each menu screen, omitted during gameplay.
// ============================================================

#define CHROME_BAR_H        56
#define CHROME_BAR_BG       0x1F4F99   // darker blue than the screen
#define CHROME_BAR_TEXT     0xFFFFFF
#define CHROME_BAR_TEXT_DIM 0xB3CDFF

// Pointers to the *currently visible* bar labels. LVGL deletes screens (and
// these labels) on transition; the LV_EVENT_DELETE callbacks below null these
// out so the periodic timer doesn't write to freed memory.
static lv_obj_t *s_clock_label = nullptr;
static lv_obj_t *s_batt_label  = nullptr;

static void clear_clock_label_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == s_clock_label) s_clock_label = nullptr;
}
static void clear_batt_label_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == s_batt_label) s_batt_label = nullptr;
}

// Plain percent text (no FontAwesome icon glyph). Used by both the LVGL
// chrome bar (which adds the icon as a glyph from Montserrat) and the DPI
// QR overlay (which draws a battery rectangle manually — NotoSansJP, used
// by the DPI text rasterizer, doesn't ship FontAwesome glyphs and would
// render LV_SYMBOL_* as tofu).
static void format_battery_pct(char *buf, size_t buflen) {
    if (!battery_is_present()) {
        snprintf(buf, buflen, " ");
        return;
    }
    int pct = battery_get_percent();
    if (battery_is_charging()) {
        snprintf(buf, buflen, "+%d%%", pct);
    } else {
        snprintf(buf, buflen, "%d%%", pct);
    }
}

static void chrome_tick_cb(lv_timer_t *t) {
    if (s_clock_label) {
        char clk[8];
        time_get_hhmm(clk, sizeof(clk));
        lv_label_set_text(s_clock_label, clk);
    }
    if (s_batt_label) {
        char b[16];
        format_battery_pct(b, sizeof(b));
        lv_label_set_text(s_batt_label, b);
    }
    // The battery icon widgets are static within a screen — they're rebuilt
    // when the screen rebuilds, so the fill bar updates whenever you navigate
    // between menu screens. Live-updating it within a single screen would
    // require tracking the rect/nub widget pointers like s_batt_label.
}

// Convenience: fill `clock`/`batt` with the current chrome-bar text. Shared
// between the LVGL-side timer (chrome_tick_cb) and the camera-side direct
// DPI overlay used during QR scan. The DPI side draws a battery rectangle
// itself, so `batt` is plain percent text without any FontAwesome glyph.
static void chrome_get_strings(char *clock, size_t clock_len,
                               char *batt,  size_t batt_len)
{
    if (clock) time_get_hhmm(clock, clock_len);
    if (batt)  format_battery_pct(batt, batt_len);
}

// Add Switch-style top + bottom bars to a screen. `hint_text` is shown in the
// bottom bar (replaces the per-screen footer hint). The top bar shows clock
// (left) and battery (right). Called from each menu screen's build_*().
static void add_chrome_bars(lv_obj_t *scr, const char *hint_text)
{
    // --- Top bar ---
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), CHROME_BAR_H);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(CHROME_BAR_BG), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    // The chrome bar uses NotoSansJP via lv_tiny_ttf (same TTF + stb_truetype
    // as the DPI direct-draw side in dsi_modal.cpp), so the QR-scan view and
    // the menu chrome render at identical pixel size. menu_font() would pick
    // pre-rasterized Montserrat in EN mode and the two paths would diverge.
    const lv_font_t *bar_font = get_jp_font(24);
    if (!bar_font) bar_font = menu_font(24);

    // Clock (left)
    lv_obj_t *clk = lv_label_create(top);
    char buf[8];
    time_get_hhmm(buf, sizeof(buf));
    lv_label_set_text(clk, buf);
    lv_obj_set_style_text_color(clk, lv_color_hex(CHROME_BAR_TEXT), 0);
    lv_obj_set_style_text_font(clk, bar_font, 0);
    lv_obj_align(clk, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_add_event_cb(clk, clear_clock_label_cb, LV_EVENT_DELETE, nullptr);
    s_clock_label = clk;

    // Battery (right) — plain percent text. The icon next to it is drawn as
    // primitive widgets below so it stays in sync with the DPI side, which
    // also draws a manual rectangle (NotoSansJP has no FontAwesome glyphs).
    lv_obj_t *batt = lv_label_create(top);
    char b2[16];
    format_battery_pct(b2, sizeof(b2));
    lv_label_set_text(batt, b2);
    lv_obj_set_style_text_color(batt, lv_color_hex(CHROME_BAR_TEXT), 0);
    lv_obj_set_style_text_font(batt, bar_font, 0);
    lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_add_event_cb(batt, clear_batt_label_cb, LV_EVENT_DELETE, nullptr);
    s_batt_label = batt;

    // Battery rect icon: 30x14 outlined body + 4x6 nub on the right + a fill
    // bar proportional to the current charge. Mirrors dsi_qr_top_bar()'s
    // primitives so both bars look identical.
    if (battery_is_present()) {
        int pct = battery_get_percent();
        bool chg = battery_is_charging();

        lv_obj_t *body = lv_obj_create(top);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, 30, 14);
        lv_obj_set_style_border_color(body, lv_color_hex(CHROME_BAR_TEXT), 0);
        lv_obj_set_style_border_width(body, 1, 0);
        lv_obj_set_style_radius(body, 2, 0);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        // Right edge of body sits a few px left of the percent label.
        lv_obj_align_to(body, batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);

        // Nub (positive terminal): a 4x6 white rect to the right of body.
        lv_obj_t *nub = lv_obj_create(top);
        lv_obj_remove_style_all(nub);
        lv_obj_set_size(nub, 4, 6);
        lv_obj_set_style_bg_color(nub, lv_color_hex(CHROME_BAR_TEXT), 0);
        lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
        lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align_to(nub, body, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

        // Fill bar inside body (inset 2 px). White normally, green when
        // charging — same convention as the DPI overlay.
        if (pct > 0) {
            int fill_w_max = 26;  // 30 body width - 4 (2 px inset each side)
            int fill_w = fill_w_max * pct / 100;
            if (fill_w > fill_w_max) fill_w = fill_w_max;
            if (fill_w < 1) fill_w = 1;
            lv_obj_t *fill = lv_obj_create(body);
            lv_obj_remove_style_all(fill);
            lv_obj_set_size(fill, fill_w, 10);
            lv_obj_set_style_bg_color(fill,
                lv_color_hex(chg ? 0x4CBF56 : CHROME_BAR_TEXT), 0);
            lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
            lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }

    // --- Bottom bar ---
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_remove_style_all(bot);
    lv_obj_set_size(bot, lv_pct(100), CHROME_BAR_H);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, lv_color_hex(CHROME_BAR_BG), 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    if (hint_text && hint_text[0]) {
        lv_obj_t *hint = lv_label_create(bot);
        lv_label_set_text(hint, hint_text);
        lv_obj_set_style_text_color(hint, lv_color_hex(CHROME_BAR_TEXT_DIM), 0);
        lv_obj_set_style_text_font(hint, menu_font(20), 0);
        lv_obj_center(hint);
    }
}

// ============================================================
// Main Menu Screen
// ============================================================

static void on_games_click(lv_event_t *e) {
    s_main_menu_focus_idx = 0;
    build_game_list();
    show_screen(s_scr_games);
    s_state = MenuState::GAME_LIST;
}

static void on_new_game_click(lv_event_t *e) {
    s_main_menu_focus_idx = 1;
    // If WiFi is connected, go straight to project QR scan
    // Otherwise, need WiFi setup first
    if (wifi_is_connected()) {
        s_state = MenuState::QR_PROJECT;
    } else {
        // Check if saved credentials exist — try connecting first
        char ssid[64] = {}, pass[128] = {};
        if (sd_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
            s_state = MenuState::QR_PROJECT; // app_main will try connecting
        } else {
            s_state = MenuState::QR_WIFI; // Need WiFi QR scan
        }
    }
}

static void on_settings_click(lv_event_t *e) {
    s_main_menu_focus_idx = 2;
    build_settings();
    show_screen(s_scr_settings);
    s_state = MenuState::SETTINGS;
}

static void build_main_menu()
{
    if (s_scr_main) {
        // old screen deleted by show_screen auto_del
    }

    // Clear group for new screen
    lv_group_remove_all_objs(s_group);

    s_scr_main = lv_obj_create(nullptr);
    lv_obj_add_style(s_scr_main, &s_style_bg, 0);

    // Logo image (replaces "ScratchESP" text — 512x128 RGB565A8)
    lv_obj_t *logo = lv_image_create(s_scr_main);
    lv_image_set_src(logo, &ui_logo);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);

    // 3-tile container (horizontal flex, big icon-on-top tiles)
    lv_obj_t *cont = lv_obj_create(s_scr_main);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, lv_pct(100), 320);
    lv_obj_align(cont, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cont, 32, 0);

    lv_obj_t *tiles[3];
    tiles[0] = create_tile_btn(cont, &ui_icon_play,     tr(STR_MAIN_GAMES),    on_games_click);
    tiles[1] = create_tile_btn(cont, &ui_icon_new,      tr(STR_MAIN_NEW_GAME), on_new_game_click);
    tiles[2] = create_tile_btn(cont, &ui_icon_settings, tr(STR_MAIN_SETTINGS), on_settings_click);

    int idx = s_main_menu_focus_idx;
    if (idx < 0 || idx > 2) idx = 0;
    lv_group_focus_obj(tiles[idx]);

    bool conn = InputDev::any_connected();
    char hint[128];
    snprintf(hint, sizeof(hint), "%s  %s%s", tr(STR_FOOTER_HINT),
             conn ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
             conn ? tr(STR_GAMEPAD_OK) : tr(STR_GAMEPAD_NONE));
    add_chrome_bars(s_scr_main, hint);
}

// ============================================================
// Game List Screen
// ============================================================

static void on_game_play(lv_event_t *e) {
    const char *pid = (const char *)lv_event_get_user_data(e);
    if (pid) {
        snprintf(s_selected_project_id, sizeof(s_selected_project_id), "%s", pid);
        s_action = MenuAction::PLAY_FROM_SD;
        s_state = MenuState::PLAYING;
    }
}

static void on_game_list_back(lv_event_t *e) {
    build_main_menu();
    show_screen(s_scr_main);
    s_state = MenuState::MAIN_MENU;
}

static void build_game_list()
{
    if (s_scr_games) {
        // old screen deleted by show_screen auto_del
    }
    lv_group_remove_all_objs(s_group);

    s_scr_games = lv_obj_create(nullptr);
    lv_obj_add_style(s_scr_games, &s_style_bg, 0);

    // Header
    lv_obj_t *title = lv_label_create(s_scr_games);
    char title_buf[96];
    snprintf(title_buf, sizeof(title_buf), LV_SYMBOL_LIST "  %s", tr(STR_GAMES_TITLE));
    lv_label_set_text(title, title_buf);
    lv_obj_add_style(title, &s_style_title, 0);
    lv_obj_set_style_text_font(title, menu_font(24), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 68);

    // Back button
    lv_obj_t *back_btn = lv_btn_create(s_scr_games);
    lv_obj_add_style(back_btn, &s_style_btn, 0);
    lv_obj_add_style(back_btn, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(back_btn, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(back_btn, 140, 44);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 20, 64);
    lv_obj_set_style_text_font(back_btn, menu_font(16), 0);
    lv_obj_add_event_cb(back_btn, on_game_list_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    char back_buf[64];
    snprintf(back_buf, sizeof(back_buf), LV_SYMBOL_LEFT " %s", tr(STR_BACK));
    lv_label_set_text(back_lbl, back_buf);
    lv_obj_center(back_lbl);
    lv_group_add_obj(s_group, back_btn);

    // Game list — keep static so project_id pointers stored as user_data
    // remain valid after build_game_list returns.
    static SdGameList games;
    sd_list_games(&games);

    if (games.count == 0) {
        lv_obj_t *empty = lv_label_create(s_scr_games);
        lv_label_set_text(empty, tr(STR_GAMES_EMPTY));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0xD9E3F7), 0);
        lv_obj_set_style_text_font(empty, menu_font(18), 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // Scrollable list container
    lv_obj_t *list = lv_obj_create(s_scr_games);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(90), 500);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < games.count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_add_style(row, &s_style_btn, 0);
        lv_obj_add_style(row, &s_style_btn_focus, LV_STATE_FOCUSED);
        lv_obj_add_style(row, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 70);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // Game name/ID (use JP-capable font so Japanese titles render)
        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, games.entries[i].name[0] ? games.entries[i].name : games.entries[i].project_id);
        lv_font_t *jp = get_jp_font(20);
        lv_obj_set_style_text_font(name, jp ? jp : &lv_font_montserrat_18, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(name, 700);

        // Store project_id pointer (static SdGameList — pointer stays valid)
        lv_obj_set_user_data(row, (void *)games.entries[i].project_id);
        lv_obj_add_event_cb(row, on_game_play, LV_EVENT_CLICKED, (void *)games.entries[i].project_id);
        lv_group_add_obj(s_group, row);
    }

    add_chrome_bars(s_scr_games, tr(STR_GAMES_HINT));
}

// ============================================================
// Settings Screen
// ============================================================

static lv_obj_t *s_brightness_value_lbl = nullptr;
static lv_obj_t *s_volume_value_lbl = nullptr;

// Stable order for the language picker. New languages can be appended here
// without breaking the settings screen layout (the settings card just shows
// the currently selected language; the picker subscreen scales to N entries).
static const struct { Lang lang; const char *display; } LANG_LIST[] = {
    { Lang::EN,    "English"  },
    { Lang::JP,    "日本語"    },
    { Lang::KR,    "한국어"    },
    { Lang::ZH_CN, "简体中文"  },
};
static const int LANG_COUNT = sizeof(LANG_LIST) / sizeof(LANG_LIST[0]);

static void on_brightness_slider(lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int step = lv_slider_get_value(slider);
    if (step < 1) step = 1;
    if (step != s_brightness_step) SoundPlayer::playSound("ui_select");
    s_brightness_step = step;
    int pct = STEP_TO_PCT(step);
    dsi_backlight_set(pct);
    if (s_brightness_value_lbl) {
        lv_label_set_text_fmt(s_brightness_value_lbl, "%d%%", pct);
    }
    sd_save_brightness(pct);
}

static void on_volume_slider(lv_event_t *e) {
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int step = lv_slider_get_value(slider);
    if (step < 0) step = 0;
    if (step != s_volume_step) SoundPlayer::playSound("ui_select");
    s_volume_step = step;
    s_muted = (step == 0);
    es8388_set_volume(step, VOLUME_MAX_STEP);
    int pct = STEP_TO_PCT(step);
    if (s_volume_value_lbl) {
        lv_label_set_text_fmt(s_volume_value_lbl, "%d%%", pct);
    }
    sd_save_volume(pct);
}

static void on_network_qr(lv_event_t *e) {
    s_qr_wifi_from_settings = true;
    s_state = MenuState::QR_WIFI;
}

static void on_settings_back(lv_event_t *e) {
    build_main_menu();
    show_screen(s_scr_main);
    s_state = MenuState::MAIN_MENU;
}

static void on_language_change(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)lv_dropdown_get_selected(dd);
    if (idx < 0 || idx >= LANG_COUNT) return;
    Lang new_lang = LANG_LIST[idx].lang;
    if (new_lang == lang_get()) return;
    lang_set(new_lang);
    build_settings();
    show_screen(s_scr_settings);
}

// Apply the JP-capable font and Scratch styling to the dropdown's popup list
// when it opens, so 日本語 / 한국어 / 简体中文 render correctly.
static void on_language_dropdown_open(lv_event_t *e) {
    lv_obj_t *dd = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (!list) return;
    lv_font_t *jp = get_jp_font(22);
    lv_obj_set_style_text_font(list, jp ? jp : &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(SCRATCH_CARD), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(SCRATCH_TEXT), 0);
    lv_obj_set_style_radius(list, 16, 0);
    lv_obj_set_style_pad_all(list, 12, 0);
    lv_obj_set_style_shadow_color(list, lv_color_hex(0x00000060), 0);
    lv_obj_set_style_shadow_width(list, 16, 0);
    // Highlight selected/focused item with Scratch orange
    lv_obj_set_style_bg_color(list, lv_color_hex(SCRATCH_ORANGE),
                              (lv_style_selector_t)LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, lv_color_hex(SCRATCH_WHITE),
                                (lv_style_selector_t)LV_PART_SELECTED | LV_STATE_CHECKED);
}

static int lang_to_index(Lang l) {
    for (int i = 0; i < LANG_COUNT; i++) {
        if (LANG_LIST[i].lang == l) return i;
    }
    return 0;
}

// Configure a chunky horizontal slider styled for kid-friendly touch use.
// Sized so the knob (track_h + pad*2 + border) fits within a 110px settings
// card minus 24px top/bottom padding (= 62px usable). Knob height = 28+10*2
// = 48px including border, leaves ~7px breathing room top/bottom.
static void style_chunky_slider(lv_obj_t *s, int min, int max, int value) {
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_height(s, 28, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_radius(s, 14, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(SCRATCH_BLUE), LV_PART_INDICATOR);
    // Circular knob: 28+10*2 = 48px diameter (fits 62px content area)
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, lv_color_hex(SCRATCH_WHITE), LV_PART_KNOB);
    lv_obj_set_style_border_color(s, lv_color_hex(SCRATCH_BLUE), LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 3, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(s, lv_color_hex(0x00000040), LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s, 4, LV_PART_KNOB);
    lv_obj_set_style_shadow_ofs_y(s, 2, LV_PART_KNOB);
    lv_obj_add_style(s, &s_style_widget_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(s, &s_style_widget_focus, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(s, &s_style_widget_focus, LV_STATE_EDITED);
}

static void build_settings()
{
    if (s_scr_settings) {
        // old screen deleted by show_screen auto_del
    }
    lv_group_remove_all_objs(s_group);
    s_brightness_value_lbl = nullptr;
    s_volume_value_lbl = nullptr;

    s_scr_settings = lv_obj_create(nullptr);
    lv_obj_add_style(s_scr_settings, &s_style_bg, 0);

    // Header — centered title, back button at top-left
    lv_obj_t *title = lv_label_create(s_scr_settings);
    lv_label_set_text(title, tr(STR_SETTINGS_TITLE));
    lv_obj_add_style(title, &s_style_title, 0);
    lv_obj_set_style_text_font(title, menu_font(28), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *back_btn = lv_btn_create(s_scr_settings);
    lv_obj_add_style(back_btn, &s_style_btn, 0);
    lv_obj_add_style(back_btn, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(back_btn, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(back_btn, 140, 48);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 20, 62);
    lv_obj_set_style_text_font(back_btn, menu_font(18), 0);
    lv_obj_add_event_cb(back_btn, on_settings_back, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    char back_buf[64];
    snprintf(back_buf, sizeof(back_buf), LV_SYMBOL_LEFT " %s", tr(STR_BACK));
    lv_label_set_text(back_lbl, back_buf);
    lv_obj_center(back_lbl);
    lv_group_add_obj(s_group, back_btn);

    // Stacked card column (1180 wide, 4 cards x 110 + 3 x 16 gap = 488 high)
    lv_obj_t *cont = lv_obj_create(s_scr_settings);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 1180, 488);
    lv_obj_align(cont, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 16, 0);

    // -- Brightness card (slider step 1..10 = 10%..100%) --
    {
        lv_obj_t *content = create_settings_card(cont, tr(STR_BRIGHTNESS));

        lv_obj_t *slider = lv_slider_create(content);
        style_chunky_slider(slider, 1, BRIGHTNESS_MAX_STEP, s_brightness_step);
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_add_event_cb(slider, on_brightness_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_group_add_obj(s_group, slider);

        s_brightness_value_lbl = lv_label_create(content);
        lv_label_set_text_fmt(s_brightness_value_lbl, "%d%%", STEP_TO_PCT(s_brightness_step));
        lv_obj_set_style_text_font(s_brightness_value_lbl, menu_font(24), 0);
        lv_obj_set_style_text_color(s_brightness_value_lbl, lv_color_hex(SCRATCH_TEXT), 0);
        lv_obj_set_style_text_align(s_brightness_value_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_brightness_value_lbl, 110);
    }

    // -- Volume card (slider 0..10 = 0%..100%, 0 = mute) --
    {
        lv_obj_t *content = create_settings_card(cont, tr(STR_VOLUME));

        lv_obj_t *slider = lv_slider_create(content);
        style_chunky_slider(slider, 0, VOLUME_MAX_STEP, s_volume_step);
        lv_obj_set_flex_grow(slider, 1);
        lv_obj_add_event_cb(slider, on_volume_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_group_add_obj(s_group, slider);

        s_volume_value_lbl = lv_label_create(content);
        lv_label_set_text_fmt(s_volume_value_lbl, "%d%%", STEP_TO_PCT(s_volume_step));
        lv_obj_set_style_text_font(s_volume_value_lbl, menu_font(24), 0);
        lv_obj_set_style_text_color(s_volume_value_lbl, lv_color_hex(SCRATCH_TEXT), 0);
        lv_obj_set_style_text_align(s_volume_value_lbl, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_volume_value_lbl, 110);
    }

    // -- Language card (chunky dropdown) --
    {
        lv_obj_t *content = create_settings_card(cont, tr(STR_LANGUAGE));

        // Build options string from LANG_LIST so adding a language doesn't
        // require touching the UI code.
        static char dd_opts[256];
        dd_opts[0] = '\0';
        for (int i = 0; i < LANG_COUNT; i++) {
            if (i > 0) strncat(dd_opts, "\n", sizeof(dd_opts) - strlen(dd_opts) - 1);
            strncat(dd_opts, LANG_LIST[i].display, sizeof(dd_opts) - strlen(dd_opts) - 1);
        }

        lv_obj_t *dd = lv_dropdown_create(content);
        lv_dropdown_set_options_static(dd, dd_opts);
        lv_dropdown_set_selected(dd, lang_to_index(lang_get()));
        lv_obj_set_flex_grow(dd, 1);
        // 40px (not 56) so the focused outline (10px outside) fits within
        // the card's 62px content area: (62-40)/2 = 11px breathing room each.
        lv_obj_set_height(dd, 40);
        lv_font_t *jp = get_jp_font(22);
        lv_obj_set_style_text_font(dd, jp ? jp : &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(SCRATCH_TEXT), 0);
        lv_obj_set_style_bg_color(dd, lv_color_hex(0xF1F4F9), 0);
        lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dd, 16, 0);
        lv_obj_set_style_border_color(dd, lv_color_hex(0xD9D9D9), 0);
        lv_obj_set_style_border_width(dd, 2, 0);
        lv_obj_set_style_pad_left(dd, 20, 0);
        lv_obj_set_style_pad_right(dd, 16, 0);
        lv_obj_add_style(dd, &s_style_widget_focus, LV_STATE_FOCUSED);
        lv_obj_add_style(dd, &s_style_widget_focus, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(dd, on_language_change, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_add_event_cb(dd, on_language_dropdown_open, LV_EVENT_READY, nullptr);
        lv_group_add_obj(s_group, dd);
    }

    // -- Wi-Fi card --
    {
        lv_obj_t *content = create_settings_card(cont, "Wi-Fi");

        lv_obj_t *icon = lv_label_create(content);
        lv_label_set_text(icon, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_font(icon, menu_font(24), 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(SCRATCH_BLUE), 0);

        char saved_ssid[64] = {}, saved_pass[128] = {};
        bool has_wifi = sd_load_wifi(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));
        lv_obj_t *ssid = lv_label_create(content);
        if (has_wifi) lv_label_set_text(ssid, saved_ssid);
        else          lv_label_set_text(ssid, tr(STR_WIFI_NONE));
        lv_obj_set_style_text_font(ssid, menu_font(20), 0);
        lv_obj_set_style_text_color(ssid, lv_color_hex(SCRATCH_TEXT), 0);
        lv_obj_set_flex_grow(ssid, 1);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);

        lv_obj_t *qr_btn = lv_btn_create(content);
        lv_obj_remove_style_all(qr_btn);
        lv_obj_set_size(qr_btn, 220, 56);
        lv_obj_set_style_bg_color(qr_btn, lv_color_hex(SCRATCH_ORANGE), 0);
        lv_obj_set_style_bg_opa(qr_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(qr_btn, 16, 0);
        lv_obj_set_style_shadow_color(qr_btn, lv_color_hex(SCRATCH_ORANGE_LT), 0);
        lv_obj_set_style_shadow_width(qr_btn, 6, 0);
        lv_obj_set_style_shadow_ofs_y(qr_btn, 2, 0);
        lv_obj_add_style(qr_btn, &s_style_btn_focus, LV_STATE_FOCUSED);
        lv_obj_add_style(qr_btn, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
        lv_obj_add_event_cb(qr_btn, on_network_qr, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *qr_lbl = lv_label_create(qr_btn);
        lv_label_set_text(qr_lbl, tr(STR_WIFI_SCAN_QR));
        lv_obj_set_style_text_font(qr_lbl, menu_font(20), 0);
        lv_obj_set_style_text_color(qr_lbl, lv_color_hex(SCRATCH_WHITE), 0);
        lv_obj_center(qr_lbl);
        lv_group_add_obj(s_group, qr_btn);
    }

    add_chrome_bars(s_scr_settings, tr(STR_SETTINGS_HINT));
}


// ============================================================
// Status / WiFi / Download screens (LVGL-based)
// ============================================================

static lv_obj_t *s_scr_status = nullptr;
static lv_obj_t *s_status_title = nullptr;
static lv_obj_t *s_status_detail = nullptr;
static lv_obj_t *s_status_spinner = nullptr;
static lv_obj_t *s_status_bar = nullptr;
static lv_obj_t *s_status_pct = nullptr;

// Build a generic status screen with title, optional spinner, optional detail
static void build_status_screen(const char *title, const char *detail, bool show_spinner)
{
    s_scr_status = nullptr;  // will be replaced; old screen deleted by lv_screen_load auto_del
    lv_group_remove_all_objs(s_group);

    s_scr_status = lv_obj_create(nullptr);
    lv_obj_add_style(s_scr_status, &s_style_bg, 0);

    // Center container
    lv_obj_t *box = lv_obj_create(s_scr_status);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_btn, 0);
    lv_obj_set_size(box, 600, 300);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_set_style_pad_all(box, 30, 0);

    // Title
    s_status_title = lv_label_create(box);
    lv_label_set_text(s_status_title, title ? title : "");
    lv_obj_set_style_text_font(s_status_title, menu_font(24), 0);
    lv_obj_set_style_text_color(s_status_title, lv_color_hex(SCRATCH_TEXT), 0);
    lv_obj_set_style_text_align(s_status_title, LV_TEXT_ALIGN_CENTER, 0);

    // Spinner
    s_status_spinner = nullptr;
    if (show_spinner) {
        s_status_spinner = lv_spinner_create(box);
        lv_spinner_set_anim_params(s_status_spinner, 1000, 270);
        lv_obj_set_size(s_status_spinner, 48, 48);
        lv_obj_set_style_arc_color(s_status_spinner, lv_color_hex(SCRATCH_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_status_spinner, lv_color_hex(0xD9D9D9), LV_PART_MAIN);
        lv_obj_set_style_arc_width(s_status_spinner, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(s_status_spinner, 6, LV_PART_MAIN);
    }

    // Progress bar (hidden by default)
    s_status_bar = lv_bar_create(box);
    lv_obj_set_width(s_status_bar, lv_pct(100));
    lv_obj_set_height(s_status_bar, 20);
    lv_bar_set_range(s_status_bar, 0, 100);
    lv_bar_set_value(s_status_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(0xD9D9D9), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_bar, lv_color_hex(SCRATCH_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_status_bar, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(s_status_bar, 10, LV_PART_INDICATOR);
    lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);

    // Percentage / status text
    s_status_pct = lv_label_create(box);
    lv_label_set_text(s_status_pct, "");
    lv_obj_set_style_text_font(s_status_pct, menu_font(14), 0);
    lv_obj_set_style_text_color(s_status_pct, lv_color_hex(SCRATCH_TEXT), 0);

    // Detail text
    s_status_detail = lv_label_create(box);
    lv_label_set_text(s_status_detail, detail ? detail : "");
    lv_obj_set_style_text_font(s_status_detail, menu_font(16), 0);
    lv_obj_set_style_text_color(s_status_detail, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(s_status_detail, LV_TEXT_ALIGN_CENTER, 0);
}

// Confirm dialog screen with A/B buttons
static volatile int s_confirm_result = -1; // -1=pending, 0=no, 1=yes

// In-game overlay support: capture current DPI FB, rotate to landscape, darken,
// then use as a full-screen background image so the game shows through.
static uint8_t *s_overlay_buf = nullptr;
static lv_image_dsc_t s_overlay_dsc;
static bool s_confirm_overlay = false;

static void capture_game_to_overlay()
{
    if (!s_overlay_buf) {
        s_overlay_buf = (uint8_t *)heap_caps_aligned_alloc(64, LVGL_BUF_SIZE,
                            MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        if (!s_overlay_buf) return;
    }

    void *dpi_fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &dpi_fb) != ESP_OK || !dpi_fb) return;

    // PPA SRM: rotate 90° (portrait 720x1280 → landscape 1280x720)
    xSemaphoreTake(s_lvgl_ppa_done, pdMS_TO_TICKS(100));
    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = dpi_fb;
    srm.in.pic_w = DSI_LCD_W;
    srm.in.pic_h = DSI_LCD_H;
    srm.in.block_w = DSI_LCD_W;
    srm.in.block_h = DSI_LCD_H;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.out.buffer = s_overlay_buf;
    srm.out.buffer_size = LVGL_BUF_SIZE;
    srm.out.pic_w = LVGL_BUF_W;
    srm.out.pic_h = LVGL_BUF_H;
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    srm.scale_x = 1.0f;
    srm.scale_y = 1.0f;
    srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t ppa_ret = ppa_do_scale_rotate_mirror(s_lvgl_ppa, &srm);
    xSemaphoreGive(s_lvgl_ppa_done);
    if (ppa_ret != ESP_OK) {
        ESP_LOGE(TAG, "overlay PPA failed: %s", esp_err_to_name(ppa_ret));
        return;
    }

    // PPA wrote directly to PSRAM bypassing CPU cache → invalidate cache
    // before CPU reads, otherwise stale data is seen.
    esp_cache_msync(s_overlay_buf, LVGL_BUF_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // Darken: ~25% brightness, RGB565
    uint16_t *p = (uint16_t *)s_overlay_buf;
    int n = LVGL_BUF_W * LVGL_BUF_H;
    for (int i = 0; i < n; i++) {
        uint16_t v = p[i];
        uint16_t r = ((v >> 11) & 0x1F) >> 2;
        uint16_t g = ((v >> 5)  & 0x3F) >> 2;
        uint16_t b = ( v        & 0x1F) >> 2;
        p[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    esp_cache_msync(s_overlay_buf, LVGL_BUF_SIZE,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    s_overlay_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_overlay_dsc.header.w = LVGL_BUF_W;
    s_overlay_dsc.header.h = LVGL_BUF_H;
    s_overlay_dsc.data_size = LVGL_BUF_SIZE;
    s_overlay_dsc.data = s_overlay_buf;
}

static void on_confirm_yes(lv_event_t *e) { s_confirm_result = 1; }
static void on_confirm_no(lv_event_t *e) { s_confirm_result = 0; }

static void build_confirm_screen(const char *title, const char *detail)
{
    // old screen deleted by show_screen auto_del
    lv_group_remove_all_objs(s_group);

    s_scr_status = lv_obj_create(nullptr);
    if (s_confirm_overlay && s_overlay_buf) {
        // Bg = darkened captured game frame
        lv_obj_set_style_bg_opa(s_scr_status, LV_OPA_TRANSP, 0);
        lv_obj_t *img = lv_image_create(s_scr_status);
        lv_image_set_src(img, &s_overlay_dsc);
        lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_style(s_scr_status, &s_style_bg, 0);
    }

    // Switch-style top/bottom bars sit above the dialog.
    {
        char hint[96];
        snprintf(hint, sizeof(hint), LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " %s   "
                 LV_SYMBOL_OK " %s   " LV_SYMBOL_CLOSE " %s",
                 tr(STR_BACK), tr(STR_YES), tr(STR_NO));
        add_chrome_bars(s_scr_status, hint);
    }

    lv_obj_t *box = lv_obj_create(s_scr_status);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_btn, 0);
    lv_obj_set_size(box, 600, 280);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 20, 0);
    lv_obj_set_style_pad_all(box, 30, 0);

    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text(ttl, title);
    lv_obj_set_style_text_font(ttl, menu_font(24), 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(SCRATCH_TEXT), 0);

    if (detail) {
        lv_obj_t *dtl = lv_label_create(box);
        lv_label_set_text(dtl, detail);
        lv_obj_set_style_text_font(dtl, menu_font(16), 0);
        lv_obj_set_style_text_color(dtl, lv_color_hex(0x888888), 0);
    }

    // Button row
    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, lv_pct(100));
    lv_obj_set_height(btn_row, 56);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 24, 0);

    // Yes button (default style; focus style highlights it when selected)
    lv_obj_t *btn_yes = lv_btn_create(btn_row);
    lv_obj_add_style(btn_yes, &s_style_btn, 0);
    lv_obj_add_style(btn_yes, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_yes, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn_yes, 180, 48);
    lv_obj_set_style_text_font(btn_yes, menu_font(18), 0);
    lv_obj_add_event_cb(btn_yes, on_confirm_yes, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl_yes = lv_label_create(btn_yes);
    char yes_buf[64];
    snprintf(yes_buf, sizeof(yes_buf), LV_SYMBOL_OK "  %s", tr(STR_YES));
    lv_label_set_text(lbl_yes, yes_buf);
    lv_obj_center(lbl_yes);
    lv_group_add_obj(s_group, btn_yes);

    // No button
    lv_obj_t *btn_no = lv_btn_create(btn_row);
    lv_obj_add_style(btn_no, &s_style_btn, 0);
    lv_obj_add_style(btn_no, &s_style_btn_focus, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_no, &s_style_btn_focus, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn_no, 180, 48);
    lv_obj_set_style_text_font(btn_no, menu_font(18), 0);
    lv_obj_add_event_cb(btn_no, on_confirm_no, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl_no = lv_label_create(btn_no);
    char no_buf[64];
    snprintf(no_buf, sizeof(no_buf), LV_SYMBOL_CLOSE "  %s", tr(STR_NO));
    lv_label_set_text(lbl_no, no_buf);
    lv_obj_center(lbl_no);
    lv_group_add_obj(s_group, btn_no);
}

// ============================================================
// Button-map screen: shows the project's auto-mapped gamepad→key pairs
// using the same kid-friendly card style as build_confirm_screen.
// ============================================================

// Pretty label for a gamepad button name (left chip).
static const char *btn_label(const std::string &k) {
    if (k == "dpadUp")    return LV_SYMBOL_UP;
    if (k == "dpadDown")  return LV_SYMBOL_DOWN;
    if (k == "dpadLeft")  return LV_SYMBOL_LEFT;
    if (k == "dpadRight") return LV_SYMBOL_RIGHT;
    if (k == "shoulderL") return "LB";
    if (k == "shoulderR") return "RB";
    if (k == "start")     return "START";
    if (k == "back")      return "BACK";
    return k.c_str(); // A, B, X, Y, LT, RT
}

// Pretty label for a Scratch key name (right side).
static std::string key_label(const std::string &k) {
    if (k == "up arrow")    return LV_SYMBOL_UP;
    if (k == "down arrow")  return LV_SYMBOL_DOWN;
    if (k == "left arrow")  return LV_SYMBOL_LEFT;
    if (k == "right arrow") return LV_SYMBOL_RIGHT;
    if (k == "space") {
        switch (lang_get()) {
            case Lang::JP:    return "スペース";
            case Lang::KR:    return "스페이스";
            case Lang::ZH_CN: return "空格";
            default:          return "Space";
        }
    }
    return k;
}

// Sort order so D-pad comes first, then face, shoulders, sticks, system.
static int btn_order(const std::string &k) {
    if (k == "dpadUp")    return 0;
    if (k == "dpadDown")  return 1;
    if (k == "dpadLeft")  return 2;
    if (k == "dpadRight") return 3;
    if (k == "A")         return 4;
    if (k == "B")         return 5;
    if (k == "X")         return 6;
    if (k == "Y")         return 7;
    if (k == "shoulderL") return 8;
    if (k == "shoulderR") return 9;
    if (k == "LT")        return 10;
    if (k == "RT")        return 11;
    if (k == "start")     return 12;
    if (k == "back")      return 13;
    return 99;
}

static void build_button_map_screen()
{
    lv_group_remove_all_objs(s_group);
    s_scr_status = lv_obj_create(nullptr);

    if (s_confirm_overlay && s_overlay_buf) {
        lv_obj_set_style_bg_opa(s_scr_status, LV_OPA_TRANSP, 0);
        lv_obj_t *img = lv_image_create(s_scr_status);
        lv_image_set_src(img, &s_overlay_dsc);
        lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_style(s_scr_status, &s_style_bg, 0);
    }

    // Card
    lv_obj_t *box = lv_obj_create(s_scr_status);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &s_style_btn, 0);
    lv_obj_set_size(box, 720, 600);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_set_style_pad_all(box, 28, 0);

    // Title
    lv_obj_t *ttl = lv_label_create(box);
    lv_label_set_text(ttl, tr(STR_BUTTON_MAP));
    lv_obj_set_style_text_font(ttl, menu_font(28), 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(SCRATCH_TEXT), 0);

    // Snapshot the mapping table sorted by btn_order.
    std::vector<std::pair<std::string, std::string>> entries(
        Input::inputControls.begin(), Input::inputControls.end());
    // Skip stick aliases — D-pad rows already cover the same arrow keys.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const std::pair<std::string, std::string> &p) {
            return p.first.rfind("LeftStick", 0) == 0 ||
                   p.first.rfind("RightStick", 0) == 0;
        }), entries.end());
    std::sort(entries.begin(), entries.end(),
        [](const std::pair<std::string, std::string> &a,
           const std::pair<std::string, std::string> &b) {
            return btn_order(a.first) < btn_order(b.first);
        });

    // Mapping list
    lv_obj_t *list = lv_obj_create(box);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (entries.empty()) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, tr(STR_BUTTON_MAP_NONE));
        lv_obj_set_style_text_font(empty, menu_font(20), 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        for (auto &e : entries) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, 60);
            lv_obj_set_style_radius(row, 14, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0xF2F4F8), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_left(row, 20, 0);
            lv_obj_set_style_pad_right(row, 20, 0);
            lv_obj_set_style_pad_column(row, 16, 0);

            // Orange chip with the gamepad button name.
            lv_obj_t *chip = lv_obj_create(row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_size(chip, 110, 44);
            lv_obj_set_style_radius(chip, 12, 0);
            lv_obj_set_style_bg_color(chip, lv_color_hex(SCRATCH_ORANGE), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_shadow_color(chip, lv_color_hex(SCRATCH_ORANGE_LT), 0);
            lv_obj_set_style_shadow_width(chip, 6, 0);
            lv_obj_set_style_shadow_ofs_y(chip, 2, 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *bl = lv_label_create(chip);
            lv_label_set_text(bl, btn_label(e.first));
            lv_obj_set_style_text_font(bl, menu_font(20), 0);
            lv_obj_set_style_text_color(bl, lv_color_hex(SCRATCH_WHITE), 0);
            lv_obj_center(bl);

            // Arrow separator.
            lv_obj_t *arr = lv_label_create(row);
            lv_label_set_text(arr, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_font(arr, menu_font(18), 0);
            lv_obj_set_style_text_color(arr, lv_color_hex(0xB0B6C0), 0);

            // Scratch key label.
            lv_obj_t *kl = lv_label_create(row);
            std::string kt = key_label(e.second);
            lv_label_set_text(kl, kt.c_str());
            lv_obj_set_style_text_font(kl, menu_font(22), 0);
            lv_obj_set_style_text_color(kl, lv_color_hex(SCRATCH_TEXT), 0);
        }
    }

    // Bottom hint ("B: Close")
    lv_obj_t *hint = lv_label_create(box);
    lv_label_set_text(hint, tr(STR_BUTTON_MAP_HINT));
    lv_obj_set_style_text_font(hint, menu_font(16), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
}

// ============================================================
// Screen transition helper
// ============================================================

static void show_screen(lv_obj_t *scr)
{
    if (!scr) return;
    // auto_del=true: LVGL safely deletes old screen after transition
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, true);
}

// ============================================================
// Public API
// ============================================================

void ui_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;

    // Initialize LVGL
    lv_init();
    lv_tick_set_cb(lvgl_tick_get_cb);

    // Allocate landscape draw buffer in PSRAM (1280x720 RGB565)
    s_lvgl_buf = (uint8_t *)heap_caps_aligned_alloc(64, LVGL_BUF_SIZE,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (!s_lvgl_buf) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer (%d bytes)", LVGL_BUF_SIZE);
        return;
    }
    memset(s_lvgl_buf, 0, LVGL_BUF_SIZE);

    // Register PPA SRM client for flush rotation
    {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        ESP_ERROR_CHECK(ppa_register_client(&cfg, &s_lvgl_ppa));
        ppa_event_callbacks_t cbs = { .on_trans_done = lvgl_ppa_done_cb };
        ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_lvgl_ppa, &cbs));
        s_lvgl_ppa_done = xSemaphoreCreateBinary();
        xSemaphoreGive(s_lvgl_ppa_done);
    }

    // Create display in landscape (1280x720)
    s_disp = lv_display_create(LVGL_BUF_W, LVGL_BUF_H);

    // DIRECT mode: full-size buffer, but LVGL only re-renders dirty regions.
    // Saves the cost of repainting unchanged areas (icons/text/shadows) every frame
    // during focus transitions. Flush still uses full-buffer PPA SRM for simplicity.
    lv_display_set_buffers(s_disp, s_lvgl_buf, nullptr, LVGL_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    // Flush callback (PPA rotate 270° → portrait DPI FB)
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);

    // Init styles
    init_styles();

    // Load persisted brightness/volume from SD (if present) and apply.
    // Both are stored as percent (0..100). Convert to internal 10%-step units.
    {
        int v;
        if (sd_load_brightness(&v) && v >= 10 && v <= 100) {
            s_brightness_step = v / 10;
            if (s_brightness_step < 1) s_brightness_step = 1;
            dsi_backlight_set(STEP_TO_PCT(s_brightness_step));
        } else {
            dsi_backlight_set(STEP_TO_PCT(s_brightness_step));
        }
        if (sd_load_volume(&v) && v >= 0 && v <= 100) {
            s_volume_step = v / 10;
            if (s_volume_step > VOLUME_MAX_STEP) s_volume_step = VOLUME_MAX_STEP;
            s_muted = (s_volume_step == 0);
        }
        es8388_set_volume(s_volume_step, VOLUME_MAX_STEP);
    }

    // Input device (gamepad as keypad)
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(s_indev, lvgl_indev_read_cb);

    // Touch pointer indev (ST7123 capacitive). Lives alongside the keypad
    // indev so users can tap menu items directly OR navigate with the d-pad.
    s_indev_touch = lv_indev_create();
    lv_indev_set_type(s_indev_touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev_touch, lvgl_pointer_read_cb);
    lv_indev_set_display(s_indev_touch, s_disp);

    // Default group for all focusable widgets
    s_group = lv_group_create();
    lv_group_set_default(s_group);
    // Default: PREV/NEXT navigate, ENTER enters edit mode for sliders
    lv_indev_set_group(s_indev, s_group);
    // Cursor sfx fires here on every focus move — gated by s_user_nav_pending
    lv_group_set_focus_cb(s_group, ui_group_focus_cb);

    // Mutex for LVGL thread safety
    s_lvgl_mutex = xSemaphoreCreateMutex();

    // Start LVGL handler task
    xTaskCreatePinnedToCore(lvgl_task_fn, "lvgl", 16384, nullptr, 4, &s_lvgl_task, 1);

    // Periodic refresh for the chrome bars (clock + battery). Runs in the LVGL
    // task context so label writes are safe without taking s_lvgl_mutex.
    lv_timer_create(chrome_tick_cb, 5000, nullptr);

    // Preload UI sound effects (cursor/select/cancel)
    load_ui_sounds();

    ESP_LOGI(TAG, "LVGL UI initialized (landscape %dx%d via 270 rotation)",
             DSI_LANDSCAPE_W, DSI_LANDSCAPE_H);
}

MenuAction ui_menu_run()
{
    s_action = MenuAction::NONE;
    s_state = MenuState::MAIN_MENU;

    // Build main menu (LVGL task may be suspended from previous game)
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_main_menu();
    show_screen(s_scr_main);
    xSemaphoreGive(s_lvgl_mutex);

    // Ensure LVGL flush is enabled
    s_flush_disabled = false;

    // Menu loop: wait for state changes from LVGL callbacks
    while (s_action == MenuAction::NONE) {
        // Handle states that require leaving LVGL (QR scan, etc.)
        if (s_state == MenuState::QR_WIFI) {
            // WiFi QR scan — disable LVGL flush (camera owns DPI FB)
            s_flush_disabled = true;
            dsi_clear_both_fbs(s_panel);

            bool wifi_ok = false;
            char ssid[64] = {}, pass[128] = {};

            if (camera_init()) {
                // Camera fills the full DPI FB; bars are composited on top
                // each frame and the back buffer is presented atomically
                // (num_fbs=2). No strip carving needed.
                const int QR_STRIP_H = 88;
                const int QR_TOP_H   = CHROME_BAR_H;
                camera_set_preview_strips(0, 0);

                char qr_buf[512];
                while (!wifi_ok) {
                    bool had_frame = camera_scan_qr(qr_buf, sizeof(qr_buf), s_panel);

                    // Composite chrome bars over the camera frame in the back
                    // buffer, then present once.
                    {
                        char clk[8], batt[16];
                        chrome_get_strings(clk, sizeof(clk), batt, sizeof(batt));
                        int pct = battery_is_present() ? battery_get_percent() : -1;
                        bool chg = battery_is_charging();
                        dsi_qr_top_bar(s_panel, clk, batt, pct, chg, QR_TOP_H);
                    }
                    dsi_qr_overlay(s_panel, tr(STR_QR_SCAN_WIFI_HEAD),
                                   tr(STR_QR_HINT_BACK), QR_STRIP_H);
                    dsi_present(s_panel);

                    if (had_frame && qr_buf[0] == 'W' && qr_buf[1] == ':') {
                        char *s = qr_buf + 2;
                        char *nl = strchr(s, '\n');
                        char *p = (char *)"";
                        if (nl) { *nl = '\0'; p = nl + 1; }

                        dsi_modal_show(s_panel, tr(STR_WIFI_CONNECTING), s);
                        if (wifi_connect(s, p, 15000)) {
                            wifi_ok = true;
                            snprintf(ssid, sizeof(ssid), "%.63s", s);
                            snprintf(pass, sizeof(pass), "%.127s", p);
                        } else {
                            dsi_modal_show(s_panel, tr(STR_WIFI_FAILED),
                                           tr(STR_WIFI_TRY_AGAIN));
                            vTaskDelay(pdMS_TO_TICKS(2000));
                        }
                    }
                    InputDev::State gs = InputDev::get();
                    if (gs.buttons & (InputDev::B | InputDev::BACK | InputDev::START)) break;
                    // Tap on the bottom hint strip (where the "press B to back"
                    // text lives) acts as a back gesture too — touch users
                    // otherwise have no way out of the QR scan loop.
                    // Hint strip in landscape = bottom 88 px → panel_x > (720 - QR_STRIP_H).
                    static bool s_qr_was_pressed_w = false;
                    touch_raw_t tr_w;
                    if (touch_input_get_raw(&tr_w)) {
                        bool edge = tr_w.pressed && !s_qr_was_pressed_w;
                        s_qr_was_pressed_w = tr_w.pressed;
                        if (edge && tr_w.x > (720 - QR_STRIP_H)) break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                camera_deinit();
            }

            // Re-enable LVGL flush
            s_flush_disabled = false;

            if (wifi_ok) {
                ui_show_wifi_result(true, ssid);
                sd_save_wifi(ssid, pass);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            // Drain any held B/Back/Start so the menu loop's edge-detector
            // doesn't see the QR-exit press as a fresh "back to main menu".
            while (InputDev::get().buttons & (InputDev::B | InputDev::BACK | InputDev::START)) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            // Return to whichever screen launched the QR scan.
            xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
            if (s_qr_wifi_from_settings) {
                s_qr_wifi_from_settings = false;
                build_settings();
                show_screen(s_scr_settings);
                s_state = MenuState::SETTINGS;
            } else {
                build_main_menu();
                show_screen(s_scr_main);
                s_state = MenuState::MAIN_MENU;
            }
            xSemaphoreGive(s_lvgl_mutex);
            continue;
        }

        if (s_state == MenuState::QR_PROJECT) {
            // Check WiFi (post-game reconnect runs synchronously in main, so this is rare)
            if (!wifi_is_connected()) {
                ui_show_status(tr(STR_NO_WIFI), tr(STR_NO_WIFI_DETAIL));
                vTaskDelay(pdMS_TO_TICKS(2000));
                s_state = MenuState::MAIN_MENU;
                xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
                build_main_menu();
                show_screen(s_scr_main);
                xSemaphoreGive(s_lvgl_mutex);
                continue;
            }

            // Disable LVGL flush during camera (camera owns DPI FB)
            s_flush_disabled = true;
            dsi_clear_both_fbs(s_panel);

            bool got_project = false;
            ESP_LOGI(TAG, "QR: camera_init...");
            if (camera_init()) {
                ESP_LOGI(TAG, "QR: camera ready, scanning...");
                const int QR_STRIP_H = 88;
                const int QR_TOP_H   = CHROME_BAR_H;
                camera_set_preview_strips(0, 0);

                char qr_buf[512];
                while (!got_project) {
                    bool had_frame = camera_scan_qr(qr_buf, sizeof(qr_buf), s_panel);

                    {
                        char clk[8], batt[16];
                        chrome_get_strings(clk, sizeof(clk), batt, sizeof(batt));
                        int pct = battery_is_present() ? battery_get_percent() : -1;
                        bool chg = battery_is_charging();
                        dsi_qr_top_bar(s_panel, clk, batt, pct, chg, QR_TOP_H);
                    }
                    dsi_qr_overlay(s_panel, tr(STR_QR_SCAN_PROJECT_HEAD),
                                   tr(STR_QR_HINT_BACK), QR_STRIP_H);
                    dsi_present(s_panel);

                    if (had_frame && qr_buf[0] == 'S' && qr_buf[1] == ':') {
                        snprintf(s_selected_project_id, sizeof(s_selected_project_id), "%.63s", qr_buf + 2);
                        got_project = true;
                        ESP_LOGI(TAG, "QR: GOT PROJECT %s", s_selected_project_id);
                    }
                    if (got_project) break;
                    InputDev::State gs = InputDev::get();
                    if (gs.buttons & (InputDev::B | InputDev::BACK | InputDev::START)) {
                        ESP_LOGI(TAG, "QR: back button pressed");
                        break;
                    }
                    // Tap on the bottom hint strip = back (same as WiFi scan).
                    static bool s_qr_was_pressed_p = false;
                    touch_raw_t tr_p;
                    if (touch_input_get_raw(&tr_p)) {
                        bool edge = tr_p.pressed && !s_qr_was_pressed_p;
                        s_qr_was_pressed_p = tr_p.pressed;
                        if (edge && tr_p.x > (720 - QR_STRIP_H)) {
                            ESP_LOGI(TAG, "QR: tap-back");
                            break;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                ESP_LOGI(TAG, "QR: calling camera_deinit...");
                camera_deinit();
                ESP_LOGI(TAG, "QR: camera_deinit OK");
            } else {
                ESP_LOGE(TAG, "QR: camera_init FAILED");
            }

            // Re-enable LVGL flush
            s_flush_disabled = false;

            if (got_project) {
                ESP_LOGI(TAG, "QR: project=%s", s_selected_project_id);
                s_action = MenuAction::PLAY_FROM_QR;
                s_state = MenuState::DOWNLOADING;
                break;
            }

            // Drain any held B/Back/Start so the menu loop's edge-detector
            // doesn't fire on the just-released QR-exit press.
            while (InputDev::get().buttons & (InputDev::B | InputDev::BACK | InputDev::START)) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            // Back to menu
            s_state = MenuState::MAIN_MENU;
            xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
            build_main_menu();
            show_screen(s_scr_main);
            xSemaphoreGive(s_lvgl_mutex);
            continue;
        }

        if (s_state == MenuState::PLAYING) {
            // Game selected (from SD or QR)
            break;
        }

        // Global B handler. Confirm/QR screens handle B themselves.
        // Priority: focused open dropdown → close it; otherwise → back one screen.
        // indev no longer sends ESC, so this branch is the sole owner of B.
        {
            static bool s_b_prev = false;
            bool b_now = (InputDev::get().buttons & InputDev::B) != 0;
            if (b_now && !s_b_prev) {
                bool consumed = false;
                if (s_state == MenuState::SETTINGS && s_group) {
                    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
                    lv_obj_t *focused = lv_group_get_focused(s_group);
                    if (focused && lv_obj_check_type(focused, &lv_dropdown_class)
                        && lv_dropdown_is_open(focused)) {
                        lv_dropdown_close(focused);
                        consumed = true;
                    }
                    xSemaphoreGive(s_lvgl_mutex);
                    if (consumed) {
                        SoundPlayer::playSound("ui_cancel");
                        while (InputDev::get().buttons & InputDev::B) vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
                if (!consumed &&
                    (s_state == MenuState::GAME_LIST || s_state == MenuState::SETTINGS)) {
                    SoundPlayer::playSound("ui_cancel");
                    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
                    build_main_menu();
                    show_screen(s_scr_main);
                    xSemaphoreGive(s_lvgl_mutex);
                    s_state = MenuState::MAIN_MENU;
                    while (InputDev::get().buttons & InputDev::B) vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            s_b_prev = b_now;
        }

        // Edge-detect X press in game list → delete focused entry
        if (s_state == MenuState::GAME_LIST) {
            static bool s_x_prev = false;
            bool x_now = (InputDev::get().buttons & InputDev::X) != 0;
            if (x_now && !s_x_prev) {
                xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
                lv_obj_t *focused = lv_group_get_focused(s_group);
                const char *pid = focused ? (const char *)lv_obj_get_user_data(focused) : nullptr;
                xSemaphoreGive(s_lvgl_mutex);
                if (pid && pid[0]) {
                    char detail[128];
                    snprintf(detail, sizeof(detail), tr(STR_DELETE_FMT), pid);
                    if (ui_show_confirm(tr(STR_CONFIRM), detail)) {
                        sd_delete_game(pid);
                    }
                    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
                    build_game_list();
                    show_screen(s_scr_games);
                    xSemaphoreGive(s_lvgl_mutex);
                    // Wait for X release before continuing
                    while (InputDev::get().buttons & InputDev::X) vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
            s_x_prev = x_now;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return s_action;
}

const char *ui_get_selected_project_id()
{
    return s_selected_project_id;
}

void ui_suspend()
{
    ESP_LOGW("BLUE_DBG", "===== ui_suspend (flush off) =====");
    s_flush_disabled = true;
}

void ui_resume()
{
    ESP_LOGW("BLUE_DBG", "===== ui_resume (flush on) =====");
    s_flush_disabled = false;
    // Force full redraw on next LVGL tick
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    lv_obj_invalidate(lv_screen_active());
    xSemaphoreGive(s_lvgl_mutex);
}

bool ui_show_exit_confirm()
{
    // Reuse the same LVGL confirm dialog as the menu, but show the (darkened)
    // current game frame as the background so it looks like an overlay.
    bool was_disabled = s_flush_disabled;
    capture_game_to_overlay();
    s_confirm_overlay = true;

    // Build & load the confirm screen *before* re-enabling flush, and skip the
    // fade animation. Otherwise the LVGL buffer (still holding the prior
    // "STARTING..." status screen) gets pushed for a frame and flashes.
    s_confirm_result = -1;
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_confirm_screen(tr(STR_RETURN_TO_MENU), nullptr);
    lv_screen_load_anim(s_scr_status, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    xSemaphoreGive(s_lvgl_mutex);

    s_flush_disabled = false;

    while (s_confirm_result < 0) {
        InputDev::State gs = InputDev::get();
        if (gs.buttons & InputDev::B) {
            SoundPlayer::playSound("ui_cancel");
            while (InputDev::get().buttons & InputDev::B) vTaskDelay(pdMS_TO_TICKS(20));
            s_confirm_result = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    bool result = (s_confirm_result == 1);

    s_confirm_overlay = false;
    s_flush_disabled = was_disabled;
    return result;
}

void ui_show_button_map()
{
    // Same flow as ui_show_exit_confirm: capture the current game frame as a
    // darkened backdrop, then load an LVGL screen that styles the button map
    // as a kid-friendly card matching the start-menu look.
    bool was_disabled = s_flush_disabled;
    capture_game_to_overlay();
    s_confirm_overlay = true;

    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_button_map_screen();
    lv_screen_load_anim(s_scr_status, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
    xSemaphoreGive(s_lvgl_mutex);

    s_flush_disabled = false;

    // Wait for B / BACK / START to dismiss (any "exit" intent).
    while (true) {
        InputDev::State gs = InputDev::get();
        if (gs.buttons & (InputDev::B | InputDev::BACK | InputDev::START)) break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    SoundPlayer::playSound("ui_cancel");
    while (InputDev::get().buttons & (InputDev::B | InputDev::BACK | InputDev::START)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_confirm_overlay = false;
    s_flush_disabled = was_disabled;
}

// ============================================================
// LVGL status screen APIs
// ============================================================

void ui_show_wifi_connecting(const char *ssid)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_status_screen(tr(STR_WIFI_CONNECTING), ssid, true);
    lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
    show_screen(s_scr_status);
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_show_wifi_result(bool ok, const char *ssid)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    if (s_status_title) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s  %s",
                 ok ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE,
                 ok ? tr(STR_WIFI_CONNECTED) : tr(STR_WIFI_FAILED));
        lv_label_set_text(s_status_title, buf);
        lv_obj_set_style_text_color(s_status_title,
            lv_color_hex(ok ? 0x0FBD8C : 0xFF6680), 0);  // Scratch green / red
    }
    if (s_status_spinner) {
        lv_obj_add_flag(s_status_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_status_detail) {
        lv_label_set_text(s_status_detail, ssid ? ssid : "");
    }
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_show_download_start(const char *project_id)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    char buf[96];
    snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD "  %s", tr(STR_DOWNLOADING));
    build_status_screen(buf, project_id, false);
    // Show progress bar
    if (s_status_bar) {
        lv_obj_clear_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_status_bar, 0, LV_ANIM_OFF);
    }
    show_screen(s_scr_status);
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_download_update(const char *status, int current, int total)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    if (s_status_bar && total > 0) {
        int pct = (int)((int64_t)100 * current / total);
        lv_bar_set_value(s_status_bar, pct, LV_ANIM_ON);
    }
    if (s_status_pct) {
        if (total > 0) {
            lv_label_set_text_fmt(s_status_pct, "%s  %d / %d", status ? status : "", current, total);
        } else {
            lv_label_set_text_fmt(s_status_pct, "%s  %d ...", status ? status : "", current);
        }
    }
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_show_download_result(bool ok)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    if (s_status_title) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s  %s",
                 ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                 ok ? tr(STR_DL_COMPLETE) : tr(STR_DL_FAILED));
        lv_label_set_text(s_status_title, buf);
        lv_obj_set_style_text_color(s_status_title,
            lv_color_hex(ok ? 0x0FBD8C : 0xFF6680), 0);
    }
    if (s_status_bar) {
        if (ok) lv_bar_set_value(s_status_bar, 100, LV_ANIM_ON);
    }
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_show_status(const char *title, const char *detail)
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_status_screen(title, detail, false);
    lv_obj_add_flag(s_status_bar, LV_OBJ_FLAG_HIDDEN);
    show_screen(s_scr_status);
    xSemaphoreGive(s_lvgl_mutex);
}

void ui_show_splash()
{
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    lv_group_remove_all_objs(s_group);

    s_scr_status = lv_obj_create(nullptr);
    lv_obj_add_style(s_scr_status, &s_style_bg, 0);

    lv_obj_t *logo = lv_image_create(s_scr_status);
    lv_image_set_src(logo, &ui_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -20);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *spinner = lv_spinner_create(s_scr_status);
    lv_spinner_set_anim_params(spinner, 1000, 270);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 110);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(SCRATCH_WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x88B5FF), LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);

    show_screen(s_scr_status);
    xSemaphoreGive(s_lvgl_mutex);
}

bool ui_show_confirm(const char *title, const char *detail)
{
    s_confirm_result = -1;
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
    build_confirm_screen(title, detail);
    show_screen(s_scr_status);
    xSemaphoreGive(s_lvgl_mutex);

    // Block until user responds
    while (s_confirm_result < 0) {
        // Also check raw input for B = cancel
        InputDev::State gs = InputDev::get();
        if (gs.buttons & InputDev::B) {
            SoundPlayer::playSound("ui_cancel");
            while (InputDev::get().buttons & InputDev::B) vTaskDelay(pdMS_TO_TICKS(20));
            s_confirm_result = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return s_confirm_result == 1;
}
