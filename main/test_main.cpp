#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "usb_test";

static void usb_write(const char *str) {
    usb_serial_jtag_write_bytes((const uint8_t *)str, strlen(str), pdMS_TO_TICKS(1000));
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "USB Serial/JTAG echo test");

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 65536,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));

    vTaskDelay(pdMS_TO_TICKS(1000));
    usb_write("ECHO_READY\n");

    uint8_t buf[64];
    int total = 0;
    while (true) {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (len > 0) {
            total += len;
            // Echo back what we received
            usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(1000));

            char msg[64];
            snprintf(msg, sizeof(msg), "\n[RX %d bytes, total %d]\n", len, total);
            usb_write(msg);
        } else {
            char msg[32];
            snprintf(msg, sizeof(msg), "WAIT:%d\n", total);
            usb_write(msg);
        }
    }
}
