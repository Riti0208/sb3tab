#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>

// Initialize WiFi subsystem (esp_hosted SDIO to C6)
// Must be called once before wifi_connect()
void wifi_init();

// Connect to WiFi AP. Blocks until connected or timeout.
// Returns true if connected successfully.
bool wifi_connect(const char *ssid, const char *password, int timeout_ms = 15000);

// Check if WiFi is connected and has IP
bool wifi_is_connected();

// Disconnect and stop WiFi (frees SDIO resources)
void wifi_disconnect();

// Download data from URL via HTTP GET.
// progress_cb(bytes_received, total_bytes) is called periodically.
// total_bytes may be 0 if Content-Length is unknown.
// Returns allocated buffer (caller must heap_caps_free) and sets out_len.
// Returns nullptr on failure.
uint8_t *wifi_http_get(const char *url, size_t *out_len,
                       std::function<void(size_t, size_t)> progress_cb = nullptr,
                       bool skip_cert_check = false);

// Persistent HTTP session that reuses one TLS connection across many GETs to
// the same host. Avoids per-request TLS handshake (the dominant cost for
// downloading many small Scratch assets).
struct WifiHttpSession;

// Open a session targeting `seed_url` (only the scheme/host/port matter; the
// initial path is replaced by wifi_http_session_get). Returns nullptr on
// failure.
WifiHttpSession *wifi_http_session_open(const char *seed_url);

// Issue a GET on `url` over the session. Same return semantics as
// wifi_http_get. URL must point at the same host/port as seed_url.
uint8_t *wifi_http_session_get(WifiHttpSession *s, const char *url, size_t *out_len,
                               std::function<void(size_t, size_t)> progress_cb = nullptr);

// Close and free the session.
void wifi_http_session_close(WifiHttpSession *s);
