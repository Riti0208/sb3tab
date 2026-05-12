#pragma once
#include "esp_lcd_panel_ops.h"
#include "input_device.h"

// Menu system states
enum class MenuState {
    MAIN_MENU,
    GAME_LIST,
    QR_WIFI,        // QR scan for WiFi credentials
    QR_PROJECT,     // QR scan for Scratch project
    DOWNLOADING,
    SETTINGS,
    PLAYING,
    CONFIRM_EXIT,   // "Return to menu?" modal during game
    BUTTON_MAP,     // Show button mapping during game
};

// Result from menu system (what app_main should do next)
enum class MenuAction {
    NONE,
    PLAY_FROM_SD,    // Launch game from SD (project_id set)
    PLAY_FROM_QR,    // Launch game just downloaded via QR
};

// Initialize LVGL and menu system. Call after dsi_display_init().
void ui_init(esp_lcd_panel_handle_t panel);

// Run the menu loop (blocks until a game is selected).
// Returns the action to take.
MenuAction ui_menu_run();

// Get the selected project ID (valid after PLAY_FROM_SD).
const char *ui_get_selected_project_id();

// Pause LVGL rendering (before entering game mode).
void ui_suspend();

// Resume LVGL rendering (after exiting game mode).
void ui_resume();

// Show in-game overlay (called from game loop on Start/Select press).
// Returns true if user confirmed exit.
bool ui_show_exit_confirm();
void ui_show_button_map();

// LVGL status screens (called from app_main with LVGL running)
// Show WiFi connecting screen (call before wifi_connect)
void ui_show_wifi_connecting(const char *ssid);
// Update WiFi status result
void ui_show_wifi_result(bool ok, const char *ssid);

// Show download progress screen. Call ui_download_update() to update.
void ui_show_download_start(const char *project_id);
void ui_download_update(const char *status, int current, int total);
void ui_show_download_result(bool ok);

// Show a simple status screen (e.g. "Loading...", "Starting!")
void ui_show_status(const char *title, const char *detail);

// Show sb3tab boot splash (logo + spinner). Call right after ui_init()
// — stays on screen until the next ui_show_* call replaces it.
void ui_show_splash();

// Show confirmation dialog. Blocks until A or B pressed.
// Returns true if confirmed (A).
bool ui_show_confirm(const char *title, const char *detail);
