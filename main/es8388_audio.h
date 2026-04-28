#pragma once

// Initialize ES8388 codec on Tab5 for DAC output via I2S
// Must be called after dsi_display_init() (which sets up g_i2c_bus and SPK_EN)
bool es8388_audio_init();

// Test: 22050Hz tone through mixer (tests interpolation path)
void es8388_test_tone_22k();

// Set mute state (true = muted)
void es8388_set_mute(bool mute);

// Set volume level. level=0 → mute, level=1..max_level → mapped to DAC digital
// volume from quietest to 0dB. Default max_level=5 (kid-friendly stepper).
void es8388_set_volume(int level, int max_level = 5);
