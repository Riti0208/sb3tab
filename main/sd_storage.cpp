#include "sd_storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_ldo_regulator.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>

static const char *TAG = "sd";
static const char *MOUNT_POINT = "/sd";
static const char *WIFI_FILE = "/sd/wifi.txt";
static const char *LANG_FILE = "/sd/lang.txt";
static const char *BRIGHTNESS_FILE = "/sd/brightness.txt";
static const char *VOLUME_FILE = "/sd/volume.txt";
static bool s_mounted = false;

bool sd_init()
{
    if (s_mounted) return true;

    // Power SD card via LDO channel 4 (3.3V)
    esp_ldo_channel_handle_t ldo_sd = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = 4;
    ldo_cfg.voltage_mv = 3300;
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_sd);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LDO4 acquire failed (may already be on): %s", esp_err_to_name(ret));
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card = nullptr;
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted: %s, %.1f MB",
             card->cid.name, (float)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    s_mounted = true;
    return true;
}

void sd_save_wifi(const char *ssid, const char *password)
{
    if (!s_mounted) return;

    FILE *f = fopen(WIFI_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", WIFI_FILE);
        return;
    }
    fprintf(f, "%s\n%s\n", ssid, password ? password : "");
    fclose(f);
    ESP_LOGI(TAG, "WiFi credentials saved for '%s'", ssid);
}

bool sd_load_wifi(char *ssid, int ssid_size, char *password, int password_size)
{
    if (!s_mounted) return false;

    FILE *f = fopen(WIFI_FILE, "r");
    if (!f) return false;

    ssid[0] = '\0';
    password[0] = '\0';

    if (fgets(ssid, ssid_size, f)) {
        // Strip newline
        char *nl = strchr(ssid, '\n');
        if (nl) *nl = '\0';
    }
    if (fgets(password, password_size, f)) {
        char *nl = strchr(password, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);

    if (ssid[0] == '\0') return false;

    ESP_LOGI(TAG, "WiFi credentials loaded for '%s'", ssid);
    return true;
}

void sd_save_lang(const char *code)
{
    if (!s_mounted || !code) return;
    FILE *f = fopen(LANG_FILE, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", LANG_FILE);
        return;
    }
    fprintf(f, "%s\n", code);
    fclose(f);
    ESP_LOGI(TAG, "Language saved: %s", code);
}

bool sd_load_lang(char *code, int code_size)
{
    if (!s_mounted || code_size < 1) return false;
    FILE *f = fopen(LANG_FILE, "r");
    if (!f) return false;

    code[0] = '\0';
    if (fgets(code, code_size, f)) {
        char *nl = strchr(code, '\n');
        if (nl) *nl = '\0';
    }
    fclose(f);
    return code[0] != '\0';
}

static void save_int_file(const char *path, int v, const char *label)
{
    if (!s_mounted) return;
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return;
    }
    fprintf(f, "%d\n", v);
    fclose(f);
    ESP_LOGI(TAG, "%s saved: %d", label, v);
}

static bool load_int_file(const char *path, int *out)
{
    if (!s_mounted || !out) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char buf[16] = {};
    bool ok = false;
    if (fgets(buf, sizeof(buf), f)) {
        ok = sscanf(buf, "%d", out) == 1;
    }
    fclose(f);
    return ok;
}

void sd_save_brightness(int pct) { save_int_file(BRIGHTNESS_FILE, pct, "Brightness"); }
bool sd_load_brightness(int *pct) { return load_int_file(BRIGHTNESS_FILE, pct); }
void sd_save_volume(int level)   { save_int_file(VOLUME_FILE, level, "Volume"); }
bool sd_load_volume(int *level)  { return load_int_file(VOLUME_FILE, level); }

// ============================================================
// Game storage
// ============================================================

static const char *GAMES_DIR = "/sd/games";

static void ensure_games_dir()
{
    struct stat st;
    if (stat(GAMES_DIR, &st) != 0) {
        mkdir(GAMES_DIR, 0755);
    }
}

void sd_list_games(SdGameList *games)
{
    games->count = 0;
    if (!s_mounted) return;
    ensure_games_dir();

    DIR *d = opendir(GAMES_DIR);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr && games->count < SD_MAX_GAMES) {
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN) continue;
        if (ent->d_name[0] == '.') continue;

        // Verify it has project.json
        char path[320];
        snprintf(path, sizeof(path), "%s/%s/project.json", GAMES_DIR, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;

        SdGameEntry &e = games->entries[games->count];
        memset(e.project_id, 0, SD_GAME_ID_LEN);
        size_t namelen = strlen(ent->d_name);
        if (namelen >= SD_GAME_ID_LEN) namelen = SD_GAME_ID_LEN - 1;
        memcpy(e.project_id, ent->d_name, namelen);

        // Try to load name from name.txt
        snprintf(path, sizeof(path), "%s/%s/name.txt", GAMES_DIR, ent->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(e.name, SD_GAME_NAME_LEN, f)) {
                char *nl = strchr(e.name, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        } else {
            e.name[0] = '\0';
        }

        games->count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "Found %d saved games", games->count);
}

// ============================================================
// Streaming save API (used during HTTPS download to avoid holding all
// assets in PSRAM simultaneously — large projects easily exceed 22 MB).
// ============================================================

bool sd_game_begin(const char *project_id, const char *name)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "game_begin: SD not mounted");
        return false;
    }
    ensure_games_dir();

    char dir[280];
    snprintf(dir, sizeof(dir), "%s/%s", GAMES_DIR, project_id);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "game_begin: mkdir(%s): %s", dir, strerror(errno));
        return false;
    }

    char assets_dir[300];
    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", dir);
    if (mkdir(assets_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "game_begin: mkdir(%s): %s", assets_dir, strerror(errno));
        return false;
    }

    if (name && name[0]) {
        char path[320];
        snprintf(path, sizeof(path), "%s/name.txt", dir);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", name); fclose(f); }
    }
    return true;
}

bool sd_game_save_json(const char *project_id, const char *json, size_t json_len)
{
    if (!s_mounted) return false;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/project.json", GAMES_DIR, project_id);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "save_json: fopen(%s): %s", path, strerror(errno));
        return false;
    }
    size_t w = fwrite(json, 1, json_len, f);
    fclose(f);
    if (w != json_len) {
        ESP_LOGE(TAG, "save_json: short write (%zu/%zu)", w, json_len);
        return false;
    }
    return true;
}

bool sd_game_save_asset(const char *project_id, const char *asset_name,
                        const uint8_t *data, size_t len)
{
    if (!s_mounted) return false;
    char path[400];
    snprintf(path, sizeof(path), "%s/%s/assets/%s", GAMES_DIR, project_id, asset_name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "save_asset: fopen(%s): %s", path, strerror(errno));
        return false;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) {
        ESP_LOGE(TAG, "save_asset: short write %s (%zu/%zu)", asset_name, w, len);
        return false;
    }
    return true;
}

uint8_t *sd_read_asset(const char *project_id, const char *asset_name, size_t *out_len)
{
    *out_len = 0;
    if (!s_mounted) return nullptr;
    char path[400];
    snprintf(path, sizeof(path), "%s/%s/assets/%s", GAMES_DIR, project_id, asset_name);
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return nullptr; }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return nullptr; }
    size_t r = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (r != (size_t)fsize) {
        heap_caps_free(buf);
        return nullptr;
    }
    *out_len = (size_t)fsize;
    return buf;
}

char *sd_read_project_json(const char *project_id, size_t *out_len)
{
    *out_len = 0;
    if (!s_mounted) return nullptr;
    char path[320];
    snprintf(path, sizeof(path), "%s/%s/project.json", GAMES_DIR, project_id);
    FILE *f = fopen(path, "r");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return nullptr; }

    char *buf = (char *)heap_caps_malloc((size_t)fsize + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return nullptr; }
    size_t r = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (r != (size_t)fsize) { heap_caps_free(buf); return nullptr; }
    buf[fsize] = '\0';
    *out_len = (size_t)fsize;
    return buf;
}

void sd_save_game(const char *project_id, const char *name,
                  const char *project_json, size_t json_len,
                  const SdAsset *assets, int asset_count)
{
    if (!s_mounted) {
        ESP_LOGE(TAG, "save_game: SD not mounted");
        return;
    }
    ensure_games_dir();

    char dir[280];
    snprintf(dir, sizeof(dir), "%s/%s", GAMES_DIR, project_id);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "save_game: mkdir(%s) failed: %s", dir, strerror(errno));
        return;
    }

    // Save project.json
    char path[320];
    snprintf(path, sizeof(path), "%s/project.json", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "save_game: fopen(%s) failed: %s", path, strerror(errno));
        return;
    }
    size_t written = fwrite(project_json, 1, json_len, f);
    fclose(f);
    if (written != json_len) {
        ESP_LOGE(TAG, "save_game: short write on project.json (%zu/%zu)", written, json_len);
        return;
    }

    // Save name
    if (name && name[0]) {
        snprintf(path, sizeof(path), "%s/name.txt", dir);
        f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n", name); fclose(f); }
    }

    // Save assets
    char assets_dir[300];
    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", dir);
    if (mkdir(assets_dir, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "save_game: mkdir(%s) failed: %s", assets_dir, strerror(errno));
        return;
    }

    int saved = 0;
    for (int i = 0; i < asset_count; i++) {
        snprintf(path, sizeof(path), "%s/%s", assets_dir, assets[i].name);
        f = fopen(path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "save_game: fopen(%s) failed: %s", path, strerror(errno));
            continue;
        }
        size_t w = fwrite(assets[i].data, 1, assets[i].len, f);
        fclose(f);
        if (w != assets[i].len) {
            ESP_LOGE(TAG, "save_game: short write on %s (%zu/%zu)", assets[i].name, w, assets[i].len);
        } else {
            saved++;
        }
    }

    ESP_LOGI(TAG, "Game saved: %s (%d/%d assets)", project_id, saved, asset_count);
}

char *sd_load_game(const char *project_id, size_t *json_len,
                   sd_asset_cb_t asset_cb, void *ctx)
{
    if (!s_mounted) return nullptr;

    char path[320];
    snprintf(path, sizeof(path), "%s/%s/project.json", GAMES_DIR, project_id);

    FILE *f = fopen(path, "r");
    if (!f) return nullptr;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = (char *)heap_caps_malloc(fsize + 1, MALLOC_CAP_SPIRAM);
    if (!json) { fclose(f); return nullptr; }

    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);
    *json_len = (size_t)fsize;

    // Load assets
    if (asset_cb) {
        char assets_dir[300];
        snprintf(assets_dir, sizeof(assets_dir), "%s/%s/assets", GAMES_DIR, project_id);
        DIR *d = opendir(assets_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;

                char apath[576];
                snprintf(apath, sizeof(apath), "%s/%s", assets_dir, ent->d_name);
                FILE *af = fopen(apath, "rb");
                if (!af) continue;

                fseek(af, 0, SEEK_END);
                long asize = ftell(af);
                fseek(af, 0, SEEK_SET);

                uint8_t *adata = (uint8_t *)heap_caps_malloc(asize, MALLOC_CAP_SPIRAM);
                if (adata) {
                    fread(adata, 1, asize, af);
                    asset_cb(ent->d_name, adata, asize, ctx);
                    heap_caps_free(adata);
                }
                fclose(af);
            }
            closedir(d);
        }
    }

    ESP_LOGI(TAG, "Game loaded: %s (%ld bytes JSON)", project_id, fsize);
    return json;
}

static void remove_dir_recursive(const char *dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    char path[320];
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            remove_dir_recursive(path);
        } else {
            unlink(path);
        }
    }
    closedir(d);
    rmdir(dirpath);
}

void sd_delete_game(const char *project_id)
{
    if (!s_mounted) return;

    char dir[280];
    snprintf(dir, sizeof(dir), "%s/%s", GAMES_DIR, project_id);
    remove_dir_recursive(dir);
    ESP_LOGI(TAG, "Game deleted: %s", project_id);
}
