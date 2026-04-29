#include "time_sync.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>
#include <cstdio>
#include <cstdlib>

static const char *TAG = "time_sync";

static bool s_started = false;

static void on_synced(struct timeval *tv) {
    ESP_LOGI(TAG, "SNTP sync: %lld", (long long)tv->tv_sec);
}

void time_sync_start() {
    // JST. Hardcoded for now; revisit if we add a timezone setting.
    setenv("TZ", "JST-9", 1);
    tzset();

    if (s_started) {
        esp_sntp_restart();
        return;
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    cfg.sync_cb = on_synced;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed");
        return;
    }
    s_started = true;
}

bool time_is_synced() {
    // 2024-01-01 in seconds since epoch — anything less means clock is still
    // at boot default (1970/2020).
    time_t now = time(nullptr);
    return now > 1704067200;
}

void time_get_hhmm(char *buf, size_t buflen) {
    if (!buf || buflen < 6) return;
    if (!time_is_synced()) {
        snprintf(buf, buflen, "--:--");
        return;
    }
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    snprintf(buf, buflen, "%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min);
}
