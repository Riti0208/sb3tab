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

// Load a project from SD card. Returns allocated JSON string (caller frees).
// asset_cb is called for each asset file; data is valid only during callback.
typedef void (*sd_asset_cb_t)(const char *name, const uint8_t *data, size_t len, void *ctx);
char *sd_load_game(const char *project_id, size_t *json_len,
                   sd_asset_cb_t asset_cb, void *ctx);

// Delete a saved game.
void sd_delete_game(const char *project_id);
