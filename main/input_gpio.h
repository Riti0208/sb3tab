#pragma once

// GPIO buttons backend for the InputDev layer.
//
// To enable, set INPUT_GPIO_ENABLE=1 in input_gpio.cpp and fill in s_pins[]
// with your wiring. Active-low (button to GND, internal pull-up enabled).
// When disabled this is a no-op so it can stay in the build.
void input_gpio_init();
