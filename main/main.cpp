#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "esp_psram.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_task_wdt.h"

// ScratchEverywhere headers
#include <runtime.hpp>
#include <parser.hpp>
#include <unzip.hpp>
#include <render.hpp>
#include <nlohmann/json.hpp>

static const char *TAG = "scratcher";

#define WIFI_SSID      CONFIG_WIFI_SSID
#define WIFI_PASS      CONFIG_WIFI_PASSWORD

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
#define MAX_RETRY 5

// ============================================================
// Wi-Fi
// ============================================================

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta()
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &instance_got_ip));
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected to %s", WIFI_SSID);
        return true;
    }
    ESP_LOGE(TAG, "Wi-Fi connection failed");
    return false;
}

// ============================================================
// HTTP Download
// ============================================================

struct DownloadBuffer {
    uint8_t *data;
    size_t len;
    size_t capacity;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    DownloadBuffer *buf = (DownloadBuffer *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (buf->len + evt->data_len > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        while (new_cap < buf->len + evt->data_len) new_cap *= 2;
        if (new_cap > 4 * 1024 * 1024) new_cap = 4 * 1024 * 1024;
        if (new_cap < buf->len + evt->data_len) return ESP_FAIL;
        uint8_t *new_data = (uint8_t *)heap_caps_realloc(buf->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!new_data) return ESP_FAIL;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    return ESP_OK;
}

static DownloadBuffer http_get(const char *url)
{
    DownloadBuffer buf = {};
    buf.capacity = 64 * 1024;
    buf.data = (uint8_t *)heap_caps_malloc(buf.capacity, MALLOC_CAP_SPIRAM);
    if (!buf.data) return buf;

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.user_data = &buf;
    config.buffer_size = 8192;
    config.buffer_size_tx = 4096;
    config.timeout_ms = 30000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d, %zu bytes", status, buf.len);
        if (status != 200) { heap_caps_free(buf.data); buf.data = nullptr; buf.len = 0; }
    } else {
        heap_caps_free(buf.data); buf.data = nullptr; buf.len = 0;
    }
    esp_http_client_cleanup(client);
    return buf;
}

static DownloadBuffer download_project_json(const char *project_id)
{
    char api_url[128];
    snprintf(api_url, sizeof(api_url), "https://api.scratch.mit.edu/projects/%s", project_id);
    DownloadBuffer api_resp = http_get(api_url);
    if (!api_resp.data) return api_resp;

    api_resp.data[api_resp.len] = '\0';
    cJSON *root = cJSON_Parse((char *)api_resp.data);
    heap_caps_free(api_resp.data);
    api_resp.data = nullptr; api_resp.len = 0;
    if (!root) return api_resp;

    cJSON *token_item = cJSON_GetObjectItem(root, "project_token");
    if (!token_item || !cJSON_IsString(token_item)) { cJSON_Delete(root); return api_resp; }

    char project_url[512];
    snprintf(project_url, sizeof(project_url),
             "https://projects.scratch.mit.edu/%s?token=%s",
             project_id, token_item->valuestring);
    cJSON_Delete(root);
    return http_get(project_url);
}

// ============================================================
// Serial Input
// ============================================================

static std::string read_project_id_from_serial()
{
    ESP_LOGI(TAG, "Enter Scratch project ID:");
    char line[64] = {};
    int pos = 0;
    int wait_count = 0;
    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (++wait_count % 20 == 0)
                ESP_LOGI(TAG, "Waiting for input...");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (c == '\n' || c == '\r') { if (pos > 0) break; continue; }
        if (pos < (int)sizeof(line) - 1) line[pos++] = (char)c;
    }
    line[pos] = '\0';
    ESP_LOGI(TAG, "Project ID: %s", line);
    return std::string(line);
}

// ============================================================
// Main
// ============================================================

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Scratcher - ScratchEverywhere on ESP32");
    ESP_LOGI(TAG, "Free PSRAM: %zu bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!wifi_init_sta()) {
        ESP_LOGE(TAG, "Cannot proceed without Wi-Fi. Restarting...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    while (true) {
        std::string project_id = read_project_id_from_serial();

        ESP_LOGI(TAG, "Downloading project %s ...", project_id.c_str());
        DownloadBuffer proj = download_project_json(project_id.c_str());
        if (!proj.data || proj.len == 0) {
            ESP_LOGE(TAG, "Download failed");
            continue;
        }

        ESP_LOGI(TAG, "Downloaded %zu bytes", proj.len);

        // Parse JSON using nlohmann::json (what ScratchEverywhere uses)
        ESP_LOGI(TAG, "Parsing with nlohmann::json...");
        esp_task_wdt_deinit();

        nlohmann::json project_json;
        try {
            project_json = nlohmann::json::parse((char *)proj.data, (char *)proj.data + proj.len);
        } catch (const std::exception &e) {
            ESP_LOGE(TAG, "JSON parse failed: %s", e.what());
            heap_caps_free(proj.data);
            esp_task_wdt_config_t wdt_config = { .timeout_ms = 5000, .idle_core_mask = 0x3, .trigger_panic = false };
            esp_task_wdt_init(&wdt_config);
            continue;
        }
        heap_caps_free(proj.data);
        proj.data = nullptr;

        ESP_LOGI(TAG, "JSON parsed OK. Loading sprites...");

        // Use ScratchEverywhere's parser to load sprites
        Parser::loadSprites(project_json);

        ESP_LOGI(TAG, "Sprites loaded: %zu", Scratch::sprites.size());
        for (auto &sprite : Scratch::sprites) {
            ESP_LOGI(TAG, "  %s: pos=(%.1f,%.1f) dir=%.1f blocks=%zu",
                     sprite->name.c_str(), sprite->xPosition, sprite->yPosition,
                     sprite->rotation, sprite->blocks.size());
        }

        // Initialize and run
        ESP_LOGI(TAG, "=== Running green flag ===");
        Scratch::initializeScratchProject();

        // Run for a limited number of frames
        for (int frame = 0; frame < 150; frame++) {
            auto [running, restart] = Scratch::stepScratchProject();
            if (!running) break;

            // Log sprite positions every 30 frames
            if (frame % 30 == 0) {
                for (auto &sprite : Scratch::sprites) {
                    if (!sprite->isStage) {
                        ESP_LOGI(TAG, "  [frame %d] %s: (%.1f, %.1f) dir=%.1f",
                                 frame, sprite->name.c_str(),
                                 sprite->xPosition, sprite->yPosition, sprite->rotation);
                    }
                }
            }
        }

        ESP_LOGI(TAG, "=== Final state ===");
        for (auto &sprite : Scratch::sprites) {
            if (!sprite->isStage) {
                ESP_LOGI(TAG, "  %s: pos=(%.1f, %.1f) dir=%.1f",
                         sprite->name.c_str(), sprite->xPosition, sprite->yPosition, sprite->rotation);
            }
        }

        Scratch::cleanupScratchProject();

        esp_task_wdt_config_t wdt_config = { .timeout_ms = 5000, .idle_core_mask = 0x3, .trigger_panic = false };
        esp_task_wdt_init(&wdt_config);

        ESP_LOGI(TAG, "=== Done. Enter next project ID ===");
    }
}
