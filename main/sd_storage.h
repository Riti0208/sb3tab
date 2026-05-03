#pragma once
#include <cstdint>
#include <cstddef>

// Initialize SD card (SDMMC SLOT_0 4-bit, FAT, mounted at /sd)
// Returns true if mounted successfully.
bool sd_init();

// Save WiFi credentials to SD card.
void sd_save_wifi(const char *ssid, const char *password);

// Load WiFi credentials from SD card.
// Returns true if found. ssid/password buffers must be at least 64/128 bytes.
bool sd_load_wifi(char *ssid, int ssid_size, char *password, int password_size);

// Save UI language code ("en" or "jp") to SD card.
void sd_save_lang(const char *code);

// Load UI language code from SD card. Writes "en"/"jp" to code (size>=8).
// Returns true if a value was loaded.
bool sd_load_lang(char *code, int code_size);

// Save/load display brightness (0-100).
void sd_save_brightness(int pct);
bool sd_load_brightness(int *pct);

// Save/load audio volume level (0=mute, 1-5).
void sd_save_volume(int level);
bool sd_load_volume(int *level);

// Save/load autorun project ID. When `/sd/autorun.txt` exists, the boot
// flow skips the menu and launches that project; relaunches it on game
// exit so the same project loops across runs (useful for log-iteration).
// out_pid must be at least SD_GAME_ID_LEN bytes. Returns true if loaded.
void sd_save_autorun(const char *project_id);
bool sd_load_autorun(char *out_pid, int out_size);
void sd_clear_autorun();

// ============================================================
// Game storage: /sd/games/<project_id>/
// ============================================================

#define SD_MAX_GAMES 32
#define SD_GAME_NAME_LEN 64
#define SD_GAME_ID_LEN 32

struct SdGameEntry {
    char project_id[SD_GAME_ID_LEN];
    char name[SD_GAME_NAME_LEN];
};

struct SdGameList {
    SdGameEntry entries[SD_MAX_GAMES];
    int count;
};

// List saved games on SD card (populates games->entries, sets games->count).
void sd_list_games(SdGameList *games);

// Save a downloaded project to SD card.
// project_json is the raw JSON string, assets are name+data pairs.
struct SdAsset {
    const char *name;
    const uint8_t *data;
    size_t len;
};
void sd_save_game(const char *project_id, const char *name,
                  const char *project_json, size_t json_len,
                  const SdAsset *assets, int asset_count);

// Streaming save API for projects too large to hold in PSRAM.
// Use this trio during a download: begin (creates dirs + name) → save_json
// once → save_asset N times. The caller is responsible for freeing each
// asset's PSRAM buffer immediately after save_asset returns.
bool sd_game_begin(const char *project_id, const char *name);
bool sd_game_save_json(const char *project_id, const char *json, size_t json_len);
bool sd_game_save_asset(const char *project_id, const char *asset_name,
                        const uint8_t *data, size_t len);

// Read one asset from /sd/games/<id>/assets/<name>. Returns malloc'd PSRAM
// buffer (caller frees with heap_caps_free) and sets *out_len. nullptr on
// failure.
uint8_t *sd_read_asset(const char *project_id, const char *asset_name, size_t *out_len);

// Read project.json from SD only — does not touch assets. Returns
// PSRAM-allocated buffer (caller heap_caps_free) and sets *out_len.
char *sd_read_project_json(const char *project_id, size_t *out_len);

// Load a project from SD card. Returns allocated JSON string (caller frees).
// asset_cb is called for each asset file; data is valid only during callback.
typedef void (*sd_asset_cb_t)(const char *name, const uint8_t *data, size_t len, void *ctx);
char *sd_load_game(const char *project_id, size_t *json_len,
                   sd_asset_cb_t asset_cb, void *ctx);

// Delete a saved game.
void sd_delete_game(const char *project_id);
