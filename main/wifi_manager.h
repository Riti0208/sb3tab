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
