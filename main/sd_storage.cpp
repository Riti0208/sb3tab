#include "sd_storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_ldo_regulator.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "sd";
static const char *MOUNT_POINT = "/sd";
static const char *WIFI_FILE = "/sd/wifi.txt";
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
