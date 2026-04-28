// GPIO button backend — registers itself as an InputDev::Source.
// Active-low: button between GPIO and GND, internal pull-up enabled.
//
// To use:
//   1. Set INPUT_GPIO_ENABLE to 1.
//   2. Fill in s_pins[] with {GPIO number, InputDev::Btn} entries.
//   3. Call input_gpio_init() from app_main (after input_xbox_init() is fine;
//      sources are merged regardless of order).

#include "input_gpio.h"
#include "input_device.h"

#define INPUT_GPIO_ENABLE 0

#if INPUT_GPIO_ENABLE

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "input_gpio";

struct PinMap {
    int gpio;
    InputDev::Btn btn;
};

// Example wiring (M5Stamp P4 free GPIOs: 23, 21, 19, 17, 37, 38, 35, 26, 27).
// G19 has external 10k pull-up (idle high already). G35 is BOOT strap.
// G24/25 (USB-JTAG), G26/27 (USB OTG) are NOT usable as input on Stamp P4.
static const PinMap s_pins[] = {
    // {GPIO_NUM_21, InputDev::DPAD_UP},
    // {GPIO_NUM_19, InputDev::A},
};

static constexpr int s_pin_count = sizeof(s_pins) / sizeof(s_pins[0]);

static void poll(InputDev::State *out) {
    for (int i = 0; i < s_pin_count; ++i) {
        if (gpio_get_level((gpio_num_t)s_pins[i].gpio) == 0) {
            out->buttons |= s_pins[i].btn;
        }
    }
    out->any_connected = (s_pin_count > 0);
}

static bool connected() {
    return s_pin_count > 0;
}

static const char *status_msg() {
    return s_pin_count > 0 ? "GPIO" : nullptr;
}

static const InputDev::Source s_source = {
    "gpio",
    connected,
    poll,
    status_msg,
};

void input_gpio_init() {
    if (s_pin_count == 0) return;

    for (int i = 0; i < s_pin_count; ++i) {
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << s_pins[i].gpio;
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&cfg);
    }
    InputDev::register_source(&s_source);
    ESP_LOGI(TAG, "GPIO input source registered (%d pins)", s_pin_count);
}

#else // !INPUT_GPIO_ENABLE

void input_gpio_init() {
    // Disabled — no source registered.
}

#endif
