#pragma once

// Initialize SD card (SDMMC SLOT_0 4-bit, FAT, mounted at /sd)
// Returns true if mounted successfully.
bool sd_init();

// Save WiFi credentials to SD card.
void sd_save_wifi(const char *ssid, const char *password);

// Load WiFi credentials from SD card.
// Returns true if found. ssid/password buffers must be at least 64/128 bytes.
bool sd_load_wifi(char *ssid, int ssid_size, char *password, int password_size);
