#pragma once
#include "driver/i2c_master.h"

// INA226 battery monitor on M5Stack Tab5 (I2C 0x41, 5mΩ shunt).
// Init after dsi_display_init() — shares g_i2c_bus.
bool battery_monitor_init(i2c_master_bus_handle_t bus);

bool battery_is_present();
int  battery_get_voltage_mv();   // bus voltage at battery terminal
int  battery_get_current_ma();   // signed; >0 = charging into battery
int  battery_get_percent();      // 0..100, derived from voltage
bool battery_is_charging();      // best-effort from current sign
