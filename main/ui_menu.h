#pragma once
#include "esp_lcd_panel_ops.h"
#include "gamepad.h"

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
