#pragma once
#include <cstdint>

enum class Lang : uint8_t {
    EN = 0,
    JP = 1,
    KR = 2,
    ZH_CN = 3,
};

enum StringId {
    STR_FOOTER_HINT,            // "A: Select  B: Back"
    STR_GAMEPAD_OK,             // " Gamepad"
    STR_GAMEPAD_NONE,           // " No Gamepad"

    STR_MAIN_GAMES,
    STR_MAIN_NEW_GAME,
    STR_MAIN_SETTINGS,

    STR_GAMES_TITLE,
    STR_BACK,
    STR_GAMES_EMPTY,
    STR_GAMES_HINT,             // "A: Play  X: Delete  B: Back"

    STR_SETTINGS_TITLE,
    STR_BRIGHTNESS,
    STR_MUTE_AUDIO,
    STR_LANGUAGE,
    STR_NETWORK,
    STR_WIFI_SAVED_FMT,         // "Saved: %s"
    STR_WIFI_NONE,
    STR_WIFI_SCAN_QR,
    STR_SETTINGS_HINT,

    STR_YES,
    STR_NO,
    STR_CONFIRM,
    STR_DELETE_FMT,             // "Delete \"%s\"?"
    STR_RETURN_TO_MENU,

    STR_WIFI_CONNECTING,        // "Connecting to WiFi..."
    STR_WIFI_CONNECTED,         // "Connected!"
    STR_WIFI_FAILED,
    STR_DOWNLOADING,
    STR_DL_COMPLETE,
    STR_DL_FAILED,
    STR_LOADING,
    STR_LOAD_FAILED,
    STR_STARTING,
    STR_NO_WIFI,
    STR_NO_WIFI_DETAIL,
    STR_BUTTON_MAP,
    STR_BUTTON_MAP_DETAIL,
    STR_FETCHING_INFO,
    STR_LOADING_JSON,
    STR_ASSETS,

    STR_COUNT,
};

// Load the saved language from SD card (call after sd_init).
// Defaults to EN if no file exists.
void lang_init();

Lang lang_get();

// Set the active language and persist to SD.
void lang_set(Lang l);

// Translate a string id. Returns "" for unknown ids.
const char *tr(StringId id);
