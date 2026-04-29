#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include <cstring>

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group = nullptr;
static bool s_initialized = false;
static bool s_connected = false;
static SemaphoreHandle_t s_wifi_lock = nullptr;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "Disconnected from AP");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

void wifi_init()
{
    if (s_initialized) return;

    s_wifi_lock = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi subsystem initialized");
}

bool wifi_connect(const char *ssid, const char *password, int timeout_ms)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "wifi_init() not called");
        return false;
    }

    xSemaphoreTake(s_wifi_lock, portMAX_DELAY);

    if (s_connected) {
        xSemaphoreGive(s_wifi_lock);
        return true;  // already connected by a concurrent caller
    }

    s_wifi_event_group = xEventGroupCreate();

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to '%s'", ssid);
        xSemaphoreGive(s_wifi_lock);
        return true;
    }

    ESP_LOGW(TAG, "Failed to connect to '%s'", ssid);
    esp_wifi_stop();
    xSemaphoreGive(s_wifi_lock);
    return false;
}

bool wifi_is_connected()
{
    return s_connected;
}

void wifi_disconnect()
{
    if (s_wifi_lock) xSemaphoreTake(s_wifi_lock, portMAX_DELAY);
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_connected = false;
    ESP_LOGI(TAG, "WiFi disconnected");
    if (s_wifi_lock) xSemaphoreGive(s_wifi_lock);
}

// Read body of an open request into a freshly allocated PSRAM buffer.
// content_length may be -1 if unknown (chunked).
static uint8_t *read_body(esp_http_client_handle_t client, int content_length,
                          size_t *out_len,
                          const std::function<void(size_t, size_t)> &progress_cb)
{
    const bool length_known = (content_length > 0);
    size_t capacity = length_known ? (size_t)content_length : (256 * 1024);
    uint8_t *buf = (uint8_t *)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes", capacity);
        return nullptr;
    }

    size_t received = 0;
    while (true) {
        size_t want = capacity - received;
        if (want == 0) {
            // Buffer full. If we know the total length, we're done — DO NOT
            // speculatively grow at end-of-stream (the previous version did
            // and ended up storing 2x-oversized buffers, exhausting PSRAM
            // after a few hundred assets).
            if (length_known) break;
            size_t new_cap = capacity * 2;
            uint8_t *newbuf = (uint8_t *)heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!newbuf) {
                ESP_LOGE(TAG, "Realloc failed at %zu", capacity);
                heap_caps_free(buf);
                return nullptr;
            }
            buf = newbuf;
            capacity = new_cap;
            want = capacity - received;
        }
        size_t chunk = want > 8192 ? 8192 : want;
        int read_len = esp_http_client_read(client, (char *)buf + received, chunk);
        if (read_len < 0) {
            ESP_LOGW(TAG, "http_client_read returned %d after %zu bytes", read_len, received);
            break;
        }
        if (read_len == 0) break;
        received += read_len;
        if (progress_cb) {
            progress_cb(received, length_known ? (size_t)content_length : 0);
        }
    }

    *out_len = received;
    return buf;
}

// HTTP GET with streaming to PSRAM buffer (one-shot, no keep-alive)
uint8_t *wifi_http_get(const char *url, size_t *out_len,
                       std::function<void(size_t, size_t)> progress_cb,
                       bool skip_cert_check)
{
    (void)skip_cert_check;
    *out_len = 0;

    ESP_LOGI(TAG, "wifi_http_get: heap internal=%u (largest=%u) psram=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 30000;
    config.buffer_size = 16384;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return nullptr;
    }

    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return nullptr;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }

    uint8_t *buf = read_body(client, content_length, out_len, progress_cb);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!buf) return nullptr;
    ESP_LOGI(TAG, "Downloaded %zu bytes from %s", *out_len, url);
    return buf;
}

// ---------------------------------------------------------------------------
// Persistent session (HTTP keep-alive across many GETs)
// ---------------------------------------------------------------------------

struct WifiHttpSession {
    esp_http_client_handle_t client;
};

WifiHttpSession *wifi_http_session_open(const char *seed_url)
{
    ESP_LOGI(TAG, "wifi_http_session_open: %s (heap internal=%u largest=%u psram=%u)",
        seed_url,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    esp_http_client_config_t config = {};
    config.url = seed_url;
    config.timeout_ms = 30000;
    config.buffer_size = 16384;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init session client");
        return nullptr;
    }

    auto *s = new WifiHttpSession();
    s->client = client;
    return s;
}

uint8_t *wifi_http_session_get(WifiHttpSession *s, const char *url, size_t *out_len,
                               std::function<void(size_t, size_t)> progress_cb)
{
    *out_len = 0;
    if (!s || !s->client) return nullptr;

    esp_err_t err = esp_http_client_set_url(s->client, url);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_url failed: %s", esp_err_to_name(err));
        return nullptr;
    }
    esp_http_client_set_method(s->client, HTTP_METHOD_GET);
    esp_http_client_set_header(s->client, "Accept-Encoding", "identity");

    err = esp_http_client_open(s->client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session open failed: %s", esp_err_to_name(err));
        return nullptr;
    }

    int content_length = esp_http_client_fetch_headers(s->client);
    int status = esp_http_client_get_status_code(s->client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d for %s", status, url);
        esp_http_client_close(s->client);
        return nullptr;
    }

    uint8_t *buf = read_body(s->client, content_length, out_len, progress_cb);
    // Keep TCP/TLS up; close just finalizes this request when keep-alive is on.
    esp_http_client_close(s->client);
    if (!buf) return nullptr;
    ESP_LOGI(TAG, "session GET %zu bytes from %s", *out_len, url);
    return buf;
}

void wifi_http_session_close(WifiHttpSession *s)
{
    if (!s) return;
    if (s->client) esp_http_client_cleanup(s->client);
    delete s;
}
