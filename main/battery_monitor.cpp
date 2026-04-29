#include "battery_monitor.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define INA226_ADDR    0x41
#define REG_CONFIG     0x00
#define REG_SHUNT_V    0x01
#define REG_BUS_V      0x02
#define REG_DIE_ID     0xFF
#define INA226_DIE_ID  0x2260

// 16-sample average, 1.1ms vbus + 1.1ms vshunt, continuous shunt+bus mode
#define INA226_CFG     0x4527

static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_present = false;

static bool ina226_write_reg(uint8_t reg, uint16_t val) {
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(s_dev, buf, 3, 100) == ESP_OK;
}

static bool ina226_read_reg(uint8_t reg, uint16_t *val) {
    uint8_t out[2];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, out, 2, 100) != ESP_OK) return false;
    *val = ((uint16_t)out[0] << 8) | out[1];
    return true;
}

bool battery_monitor_init(i2c_master_bus_handle_t bus) {
    if (!bus) return false;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address  = INA226_ADDR;
    cfg.scl_speed_hz    = 100000;
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "INA226 add_device failed");
        return false;
    }

    // Probe DIE_ID — if the chip isn't there we silently skip (boards without battery monitor)
    uint16_t die = 0;
    if (!ina226_read_reg(REG_DIE_ID, &die) || die != INA226_DIE_ID) {
        ESP_LOGW(TAG, "INA226 not detected at 0x%02X (die=0x%04X)", INA226_ADDR, die);
        s_dev = nullptr;
        return false;
    }

    if (!ina226_write_reg(REG_CONFIG, INA226_CFG)) {
        ESP_LOGW(TAG, "INA226 config write failed");
        s_dev = nullptr;
        return false;
    }

    s_present = true;
    ESP_LOGI(TAG, "INA226 initialized (die=0x%04X)", die);
    return true;
}

bool battery_is_present() { return s_present; }

int battery_get_voltage_mv() {
    if (!s_present) return 0;
    uint16_t raw = 0;
    if (!ina226_read_reg(REG_BUS_V, &raw)) return 0;
    // LSB = 1.25 mV. raw * 1.25 = raw + raw/4 (integer).
    return (int)raw + (int)(raw >> 2);
}

int battery_get_current_ma() {
    if (!s_present) return 0;
    uint16_t raw = 0;
    if (!ina226_read_reg(REG_SHUNT_V, &raw)) return 0;
    int16_t shunt = (int16_t)raw;
    // shunt LSB = 2.5µV, R_shunt = 5mΩ → I[mA] = (shunt * 2.5e-6) / 5e-3 / 1e-3 = shunt / 2
    return shunt / 2;
}

int battery_get_percent() {
    int mv = battery_get_voltage_mv();
    if (mv < 1000) return 0;        // sensor not measuring (no battery)
    if (mv >= 4150) return 100;
    if (mv <= 3300) return 0;
    // Linear 3.30V → 4.15V mapped to 0..100
    return (mv - 3300) * 100 / (4150 - 3300);
}

bool battery_is_charging() {
    // Sign convention assumes V+ on battery side, V- on charger side; positive
    // current = flowing into battery. Empirical verification needed on first
    // boot — flip the comparison if it reads inverted.
    return battery_get_current_ma() > 20;
}
