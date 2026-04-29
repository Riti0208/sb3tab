#pragma once
#include <cstddef>

// Start SNTP and set timezone. Call after WiFi got an IP.
// Idempotent — re-arms on subsequent calls.
void time_sync_start();

bool time_is_synced();

// Format current local time as "HH:MM" (5 chars + NUL). Falls back to "--:--"
// before the first SNTP sync arrives.
void time_get_hhmm(char *buf, size_t buflen);
