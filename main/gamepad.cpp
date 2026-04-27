// Xbox controller USB Host driver for ESP32-P4
// Supports Xbox 360 (vendor class 0x5D) and Xbox One/Series (vendor class 0x47)

#include "gamepad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include <cstring>

static const char *TAG = "gamepad";

// Shared gamepad state (written by USB task, read by main loop)
static GamepadState s_state = {};
static SemaphoreHandle_t s_state_mutex = nullptr;

// Debug message buffer (visible on screen)
static char s_debug_msg[128] = "GP: waiting";

// USB host state
static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t s_device = nullptr;
static uint8_t s_ep_in = 0;
static int s_ep_in_mps = 0;
static usb_transfer_t *s_in_xfer = nullptr;
static bool s_device_connected = false;

// Controller type
enum XboxType { XBOX_NONE, XBOX_360, XBOX_ONE };
static XboxType s_xbox_type = XBOX_NONE;

#define STICK_DEADZONE 8000

// ============================================================
// Xbox report parsing
// ============================================================

static void parse_xbox360_report(const uint8_t *data, int len)
{
    if (len < 14 || data[0] != 0x00 || data[1] != 0x14) return;

    GamepadState st = {};
    st.connected = true;
    st.buttons = data[2] | (data[3] << 8);
    st.left_trigger = data[4];
    st.right_trigger = data[5];
    st.lx = (int16_t)(data[6] | (data[7] << 8));
    st.ly = (int16_t)(data[8] | (data[9] << 8));
    st.rx = (int16_t)(data[10] | (data[11] << 8));
    st.ry = (int16_t)(data[12] | (data[13] << 8));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_state_mutex);
}

static void parse_xboxone_report(const uint8_t *data, int len)
{
    if (len < 18 || data[0] != 0x20) return;

    // Xbox One GIP format:
    // [4-5] buttons, [6-7] LT, [8-9] RT, [10-17] sticks
    uint16_t raw_buttons = data[4] | (data[5] << 8);

    // Remap to unified Xbox 360 button layout
    GamepadState st = {};
    st.connected = true;
    if (raw_buttons & (1 << 8))  st.buttons |= XBOX_DPAD_UP;
    if (raw_buttons & (1 << 9))  st.buttons |= XBOX_DPAD_DOWN;
    if (raw_buttons & (1 << 10)) st.buttons |= XBOX_DPAD_LEFT;
    if (raw_buttons & (1 << 11)) st.buttons |= XBOX_DPAD_RIGHT;
    if (raw_buttons & (1 << 2))  st.buttons |= XBOX_START;
    if (raw_buttons & (1 << 3))  st.buttons |= XBOX_BACK;
    if (raw_buttons & (1 << 14)) st.buttons |= XBOX_LSTICK;
    if (raw_buttons & (1 << 15)) st.buttons |= XBOX_RSTICK;
    if (raw_buttons & (1 << 12)) st.buttons |= XBOX_LB;
    if (raw_buttons & (1 << 13)) st.buttons |= XBOX_RB;
    if (raw_buttons & (1 << 4))  st.buttons |= XBOX_A;
    if (raw_buttons & (1 << 5))  st.buttons |= XBOX_B;
    if (raw_buttons & (1 << 6))  st.buttons |= XBOX_X;
    if (raw_buttons & (1 << 7))  st.buttons |= XBOX_Y;

    uint16_t lt = data[6] | (data[7] << 8);
    uint16_t rt = data[8] | (data[9] << 8);
    st.left_trigger = lt >> 2;   // 10-bit → 8-bit
    st.right_trigger = rt >> 2;
    st.lx = (int16_t)(data[10] | (data[11] << 8));
    st.ly = (int16_t)(data[12] | (data[13] << 8));
    st.rx = (int16_t)(data[14] | (data[15] << 8));
    st.ry = (int16_t)(data[16] | (data[17] << 8));

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_state_mutex);
}

// ============================================================
// USB transfer callback
// ============================================================

static int s_xfer_dbg = 0;
static int s_xfer_ok = 0, s_xfer_fail = 0;

static void in_xfer_cb(usb_transfer_t *xfer)
{
    // Log transfer status periodically
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) s_xfer_ok++;
    else s_xfer_fail++;
    if ((s_xfer_ok + s_xfer_fail) % 200 == 1) {
        ESP_LOGI(TAG, "xfer: ok=%d fail=%d status=%d bytes=%d",
                 s_xfer_ok, s_xfer_fail, xfer->status, xfer->actual_num_bytes);
    }

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        // Log raw report when buttons pressed
        uint8_t *d = xfer->data_buffer;
        int n = xfer->actual_num_bytes;
        bool has_btn = (n >= 4 && (d[2] | d[3]) != 0);
        if (has_btn || (s_xfer_dbg++ % 500 == 0)) {
            ESP_LOGI(TAG, "raw[%d]: %02X %02X %02X %02X %02X %02X %02X %02X",
                     n, d[0], n>1?d[1]:0, n>2?d[2]:0, n>3?d[3]:0,
                     n>4?d[4]:0, n>5?d[5]:0, n>6?d[6]:0, n>7?d[7]:0);
        }
        if (s_xbox_type == XBOX_360) {
            parse_xbox360_report(xfer->data_buffer, xfer->actual_num_bytes);
        } else if (s_xbox_type == XBOX_ONE) {
            parse_xboxone_report(xfer->data_buffer, xfer->actual_num_bytes);
        }
    } else if (xfer->status == USB_TRANSFER_STATUS_STALL) {
        usb_host_endpoint_clear(s_device, s_ep_in);
    }

    // Re-submit transfer for next report
    if (s_device_connected) {
        esp_err_t err = usb_host_transfer_submit(s_in_xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Re-submit failed: %s", esp_err_to_name(err));
        }
    }
}

// ============================================================
// Xbox One init sequence
// ============================================================

static void xboxone_send_init(void)
{
    usb_transfer_t *out_xfer = nullptr;
    usb_host_transfer_alloc(8, 0, &out_xfer);
    if (!out_xfer) return;

    // Find OUT endpoint
    uint8_t ep_out = 0;
    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(s_device, &config_desc);
    const uint8_t *p = (const uint8_t *)config_desc;
    int offset = 0;
    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)(p + offset);
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)desc;
            if ((ep->bEndpointAddress & 0x80) == 0) {
                ep_out = ep->bEndpointAddress;
                break;
            }
        }
        offset += desc->bLength;
    }

    if (ep_out == 0) {
        usb_host_transfer_free(out_xfer);
        return;
    }

    // GIP init: power on controller
    uint8_t init_pkt[] = {0x05, 0x20, 0x00, 0x01, 0x00};
    memcpy(out_xfer->data_buffer, init_pkt, sizeof(init_pkt));
    out_xfer->num_bytes = sizeof(init_pkt);
    out_xfer->device_handle = s_device;
    out_xfer->bEndpointAddress = ep_out;
    out_xfer->callback = [](usb_transfer_t *xfer) {
        ESP_LOGI(TAG, "Xbox One init sent, status=%d", xfer->status);
        usb_host_transfer_free(xfer);
    };

    usb_host_transfer_submit(out_xfer);
}

// ============================================================
// Device open + claim interface + start reading
// ============================================================

static bool try_open_xbox(uint8_t dev_addr)
{
    esp_err_t ret = usb_host_device_open(s_client, dev_addr, &s_device);
    if (ret != ESP_OK) return false;

    const usb_device_desc_t *desc;
    usb_host_get_device_descriptor(s_device, &desc);
    ESP_LOGI(TAG, "USB device: VID=0x%04X PID=0x%04X class=%d",
             desc->idVendor, desc->idProduct, desc->bDeviceClass);
    snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: VID=%04X PID=%04X cls=%d",
             desc->idVendor, desc->idProduct, desc->bDeviceClass);

    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(s_device, &config_desc);

    // Walk descriptors to find Xbox interface
    const uint8_t *p = (const uint8_t *)config_desc;
    int offset = 0;
    int target_iface = -1;
    s_xbox_type = XBOX_NONE;
    s_ep_in = 0;

    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)(p + offset);

        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *iface = (const usb_intf_desc_t *)d;
            ESP_LOGI(TAG, "  iface %d: class=0x%02X sub=0x%02X proto=0x%02X",
                     iface->bInterfaceNumber, iface->bInterfaceClass,
                     iface->bInterfaceSubClass, iface->bInterfaceProtocol);
            snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: if%d c%02X s%02X p%02X",
                     iface->bInterfaceNumber, iface->bInterfaceClass,
                     iface->bInterfaceSubClass, iface->bInterfaceProtocol);

            if (iface->bInterfaceClass == 0xFF && target_iface < 0) {
                if (iface->bInterfaceSubClass == 0x5D && iface->bInterfaceProtocol == 0x01) {
                    // Xbox 360
                    target_iface = iface->bInterfaceNumber;
                    s_xbox_type = XBOX_360;
                    ESP_LOGI(TAG, "Xbox 360 controller detected (iface %d)", target_iface);
                } else if (iface->bInterfaceSubClass == 0x47 && iface->bInterfaceProtocol == 0xD0) {
                    // Xbox One / Series
                    target_iface = iface->bInterfaceNumber;
                    s_xbox_type = XBOX_ONE;
                    ESP_LOGI(TAG, "Xbox One/Series controller detected (iface %d)", target_iface);
                } else if (iface->bInterfaceSubClass == 0x47) {
                    // Xbox One variant (different protocol)
                    target_iface = iface->bInterfaceNumber;
                    s_xbox_type = XBOX_ONE;
                    ESP_LOGI(TAG, "Xbox One variant detected (iface %d, proto=0x%02X)",
                             target_iface, iface->bInterfaceProtocol);
                }
            }
        }

        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && target_iface >= 0 && s_ep_in == 0) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if (ep->bEndpointAddress & 0x80) {
                s_ep_in = ep->bEndpointAddress;
                s_ep_in_mps = ep->wMaxPacketSize;
                ESP_LOGI(TAG, "IN endpoint: 0x%02X, MPS=%d", s_ep_in, s_ep_in_mps);
            }
        }

        offset += d->bLength;
    }

    if (s_xbox_type == XBOX_NONE || s_ep_in == 0) {
        ESP_LOGW(TAG, "Not an Xbox controller or no IN endpoint");
        snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: not Xbox");
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        return false;
    }

    // Claim interface
    ret = usb_host_interface_claim(s_client, s_device, target_iface, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Interface claim failed: %s", esp_err_to_name(ret));
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        return false;
    }

    // Allocate IN transfer
    usb_host_transfer_alloc(s_ep_in_mps, 0, &s_in_xfer);
    s_in_xfer->device_handle = s_device;
    s_in_xfer->bEndpointAddress = s_ep_in;
    s_in_xfer->callback = in_xfer_cb;
    s_in_xfer->num_bytes = s_ep_in_mps;

    s_device_connected = true;
    snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: %s connected!",
             s_xbox_type == XBOX_360 ? "X360" : "XOne");

    // Xbox One needs init packet
    if (s_xbox_type == XBOX_ONE) {
        xboxone_send_init();
    }

    // Start reading
    esp_err_t sub_ret = usb_host_transfer_submit(s_in_xfer);
    ESP_LOGI(TAG, "Gamepad reading started, submit=%s", esp_err_to_name(sub_ret));
    return true;
}

// ============================================================
// USB Host client callback
// ============================================================

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New USB device: addr=%d", msg->new_dev.address);
        if (!s_device_connected) {
            try_open_xbox(msg->new_dev.address);
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected");
        s_device_connected = false;
        if (s_in_xfer) {
            usb_host_transfer_free(s_in_xfer);
            s_in_xfer = nullptr;
        }
        if (s_device) {
            usb_host_device_close(s_client, s_device);
            s_device = nullptr;
        }
        // Clear state
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memset((void *)&s_state, 0, sizeof(s_state));
        xSemaphoreGive(s_state_mutex);
        break;
    }
}

// ============================================================
// USB Host daemon + client task
// ============================================================

static void usb_host_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_client_task(void *arg)
{
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

// ============================================================
// Public API
// ============================================================

void gamepad_init()
{
    s_state_mutex = xSemaphoreCreateMutex();

    // Install USB Host library (UTMI PHY = USB-A port on Tab5)
    usb_host_config_t host_cfg = {};
    host_cfg.skip_phy_setup = false;
    host_cfg.intr_flags = ESP_INTR_FLAG_LEVEL1;

    ESP_LOGI(TAG, "Installing USB host...");
    esp_err_t ret = usb_host_install(&host_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "USB host installed OK");

    // USB host daemon task
    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, nullptr, 5, nullptr, 0);

    // Register client
    usb_host_client_config_t client_cfg = {};
    client_cfg.is_synchronous = false;
    client_cfg.max_num_event_msg = 5;
    client_cfg.async.client_event_callback = client_event_cb;
    client_cfg.async.callback_arg = nullptr;
    ret = usb_host_client_register(&client_cfg, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB client register failed: %s", esp_err_to_name(ret));
        return;
    }

    // Client event processing task
    xTaskCreatePinnedToCore(usb_client_task, "usb_gpad", 4096, nullptr, 5, nullptr, 0);

    ESP_LOGI(TAG, "Gamepad USB host initialized, waiting for controller...");
}

GamepadState gamepad_get_state()
{
    GamepadState st = {};
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        st = s_state;
        xSemaphoreGive(s_state_mutex);
    }
    return st;
}

bool gamepad_connected()
{
    return s_device_connected;
}

const char *gamepad_last_msg()
{
    return s_debug_msg;
}
