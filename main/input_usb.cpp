// USB Host gamepad driver — Xbox 360 / Xbox One / DualShock 4 / DualSense /
// generic HID gamepad — registered as a single InputDev::Source.

#include "input_usb.h"
#include "input_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "input_usb";

// ============================================================
// Constants
// ============================================================

static constexpr uint8_t  TRIGGER_DIGITAL_THRESHOLD = 128;

// Sony Interactive Entertainment
static constexpr uint16_t VID_SONY = 0x054C;
static constexpr uint16_t PID_DS4_V1 = 0x05C4;
static constexpr uint16_t PID_DS4_V2 = 0x09CC;
static constexpr uint16_t PID_DUALSENSE = 0x0CE6;

// ============================================================
// State
// ============================================================

enum DevType {
    DEV_NONE,
    DEV_XBOX_360,
    DEV_XBOX_ONE,
    DEV_DS4,
    DEV_DUALSENSE,
    DEV_HID_GENERIC,
};

static const char *dev_type_name(DevType t) {
    switch (t) {
    case DEV_XBOX_360:    return "X360";
    case DEV_XBOX_ONE:    return "XOne";
    case DEV_DS4:         return "DS4";
    case DEV_DUALSENSE:   return "DualSense";
    case DEV_HID_GENERIC: return "HID";
    default:              return "none";
    }
}

// HID descriptor parse result — mapping report bytes to gamepad fields.
// Filled in during open() for DEV_HID_GENERIC; ignored for fixed-layout pads.
struct HidLayout {
    uint8_t  report_id;          // 0 if no report ID prefix
    bool     has_report_id;

    // Bit offsets within the report (after stripping the optional report-ID
    // prefix byte). -1 = field not present.
    int16_t  bit_lx;  uint8_t sz_lx;  uint8_t sgn_lx;
    int16_t  bit_ly;  uint8_t sz_ly;  uint8_t sgn_ly;
    int16_t  bit_rx;  uint8_t sz_rx;  uint8_t sgn_rx;
    int16_t  bit_ry;  uint8_t sz_ry;  uint8_t sgn_ry;
    int16_t  bit_hat; uint8_t sz_hat;

    // Up to 16 buttons mapped sequentially starting at bit_btn0.
    int16_t  bit_btn0;
    uint8_t  n_btn;

    // Logical range for sticks (for normalization).
    int32_t  logical_min_axis;
    int32_t  logical_max_axis;

    uint16_t total_bits;
};

static InputDev::State    s_state = {};
static SemaphoreHandle_t  s_state_mutex = nullptr;
static char               s_debug_msg[128] = "GP: waiting";

static usb_host_client_handle_t s_client = nullptr;
static usb_device_handle_t s_device = nullptr;
static uint8_t  s_ep_in = 0;
static int      s_ep_in_mps = 0;
static usb_transfer_t *s_in_xfer = nullptr;
static bool     s_device_connected = false;
static DevType  s_dev_type = DEV_NONE;
static HidLayout s_hid_layout = {};

// ============================================================
// Helpers
// ============================================================

// Map an unsigned 8-bit axis (0..255, 128 center) to signed int16 range.
static inline int16_t map_axis_u8(uint8_t v) {
    return (int16_t)((int)v - 128) * 256;
}

// Map a hat-switch nibble (0..7 cardinal directions, 8 = neutral) into
// DPAD button bits OR'd into `buttons`.
static inline void map_hat(uint32_t &buttons, uint8_t hat) {
    using namespace InputDev;
    if (hat > 7) return;
    static const uint32_t lut[8] = {
        DPAD_UP,
        DPAD_UP | DPAD_RIGHT,
        DPAD_RIGHT,
        DPAD_DOWN | DPAD_RIGHT,
        DPAD_DOWN,
        DPAD_DOWN | DPAD_LEFT,
        DPAD_LEFT,
        DPAD_UP | DPAD_LEFT,
    };
    buttons |= lut[hat];
}

static inline void publish(const InputDev::State &st) {
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_state_mutex);
}

// ============================================================
// Xbox 360 (vendor class, 14-byte report)
// ============================================================

static void parse_xbox360(const uint8_t *data, int len) {
    if (len < 14 || data[0] != 0x00 || data[1] != 0x14) return;
    using namespace InputDev;
    State st = {};
    st.any_connected = true;

    uint16_t b = data[2] | (data[3] << 8);
    if (b & (1 << 0))  st.buttons |= DPAD_UP;
    if (b & (1 << 1))  st.buttons |= DPAD_DOWN;
    if (b & (1 << 2))  st.buttons |= DPAD_LEFT;
    if (b & (1 << 3))  st.buttons |= DPAD_RIGHT;
    if (b & (1 << 4))  st.buttons |= START;
    if (b & (1 << 5))  st.buttons |= BACK;
    if (b & (1 << 6))  st.buttons |= LSTICK;
    if (b & (1 << 7))  st.buttons |= RSTICK;
    if (b & (1 << 8))  st.buttons |= LB;
    if (b & (1 << 9))  st.buttons |= RB;
    if (b & (1 << 10)) st.buttons |= GUIDE;
    if (b & (1 << 12)) st.buttons |= A;
    if (b & (1 << 13)) st.buttons |= B;
    if (b & (1 << 14)) st.buttons |= X;
    if (b & (1 << 15)) st.buttons |= Y;
    if (data[4] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= LT;
    if (data[5] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= RT;

    st.lx = (int16_t)(data[6]  | (data[7]  << 8));
    st.ly = (int16_t)(data[8]  | (data[9]  << 8));
    st.rx = (int16_t)(data[10] | (data[11] << 8));
    st.ry = (int16_t)(data[12] | (data[13] << 8));
    publish(st);
}

// ============================================================
// Xbox One / Series (GIP, ≥18-byte input report 0x20)
// ============================================================

static void parse_xboxone(const uint8_t *data, int len) {
    if (len < 18 || data[0] != 0x20) return;
    using namespace InputDev;
    State st = {};
    st.any_connected = true;

    uint16_t b = data[4] | (data[5] << 8);
    if (b & (1 << 8))  st.buttons |= DPAD_UP;
    if (b & (1 << 9))  st.buttons |= DPAD_DOWN;
    if (b & (1 << 10)) st.buttons |= DPAD_LEFT;
    if (b & (1 << 11)) st.buttons |= DPAD_RIGHT;
    if (b & (1 << 2))  st.buttons |= START;
    if (b & (1 << 3))  st.buttons |= BACK;
    if (b & (1 << 14)) st.buttons |= LSTICK;
    if (b & (1 << 15)) st.buttons |= RSTICK;
    if (b & (1 << 12)) st.buttons |= LB;
    if (b & (1 << 13)) st.buttons |= RB;
    if (b & (1 << 4))  st.buttons |= A;
    if (b & (1 << 5))  st.buttons |= B;
    if (b & (1 << 6))  st.buttons |= X;
    if (b & (1 << 7))  st.buttons |= Y;

    uint16_t lt = data[6] | (data[7] << 8);
    uint16_t rt = data[8] | (data[9] << 8);
    if ((lt >> 2) > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= LT;
    if ((rt >> 2) > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= RT;

    st.lx = (int16_t)(data[10] | (data[11] << 8));
    st.ly = (int16_t)(data[12] | (data[13] << 8));
    st.rx = (int16_t)(data[14] | (data[15] << 8));
    st.ry = (int16_t)(data[16] | (data[17] << 8));
    publish(st);
}

// Xbox One GIP power-on packet — must be sent on the OUT endpoint or the
// controller stays silent.
static void xboxone_send_init(void) {
    usb_transfer_t *out_xfer = nullptr;
    usb_host_transfer_alloc(8, 0, &out_xfer);
    if (!out_xfer) return;

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
    if (ep_out == 0) { usb_host_transfer_free(out_xfer); return; }

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
// DualShock 4 — USB input report ID 0x01 (≥10 useful bytes)
// ============================================================
//
//   data[0]  = report ID (0x01)
//   data[1]  = LX  (0..255, 128 = center)
//   data[2]  = LY
//   data[3]  = RX
//   data[4]  = RY
//   data[5]  = [b7..b4 face buttons | b3..b0 hat (0..7 dir, 8 = idle)]
//   data[6]  = L1/R1/L2/R2/Share/Options/L3/R3
//   data[7]  = PS / Touchpad-click / counter
//   data[8]  = L2 analog (0..255)
//   data[9]  = R2 analog
static void parse_ds4(const uint8_t *data, int len) {
    if (len < 10 || data[0] != 0x01) return;
    using namespace InputDev;
    State st = {};
    st.any_connected = true;

    st.lx = map_axis_u8(data[1]);
    st.ly = map_axis_u8(data[2]);
    st.rx = map_axis_u8(data[3]);
    st.ry = map_axis_u8(data[4]);

    uint8_t b5 = data[5];
    map_hat(st.buttons, b5 & 0x0F);
    if (b5 & (1 << 4)) st.buttons |= X;      // Square
    if (b5 & (1 << 5)) st.buttons |= A;      // Cross
    if (b5 & (1 << 6)) st.buttons |= B;      // Circle
    if (b5 & (1 << 7)) st.buttons |= Y;      // Triangle

    uint8_t b6 = data[6];
    if (b6 & (1 << 0)) st.buttons |= LB;
    if (b6 & (1 << 1)) st.buttons |= RB;
    if (b6 & (1 << 2)) st.buttons |= LT;
    if (b6 & (1 << 3)) st.buttons |= RT;
    if (b6 & (1 << 4)) st.buttons |= BACK;   // Share
    if (b6 & (1 << 5)) st.buttons |= START;  // Options
    if (b6 & (1 << 6)) st.buttons |= LSTICK;
    if (b6 & (1 << 7)) st.buttons |= RSTICK;

    uint8_t b7 = data[7];
    if (b7 & (1 << 0)) st.buttons |= GUIDE;  // PS

    if (data[8] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= LT;
    if (data[9] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= RT;
    publish(st);
}

// ============================================================
// DualSense — USB input report ID 0x01 (similar to DS4 but shifted)
// ============================================================
//
//   data[0]  = report ID (0x01)
//   data[1]  = LX
//   data[2]  = LY
//   data[3]  = RX
//   data[4]  = RY
//   data[5]  = L2 analog
//   data[6]  = R2 analog
//   data[7]  = counter (ignored)
//   data[8]  = [b7..b4 face buttons | b3..b0 hat]
//   data[9]  = L1/R1/L2/R2/Create/Options/L3/R3
//   data[10] = PS / Touchpad-click / Mute
static void parse_dualsense(const uint8_t *data, int len) {
    if (len < 11 || data[0] != 0x01) return;
    using namespace InputDev;
    State st = {};
    st.any_connected = true;

    st.lx = map_axis_u8(data[1]);
    st.ly = map_axis_u8(data[2]);
    st.rx = map_axis_u8(data[3]);
    st.ry = map_axis_u8(data[4]);

    uint8_t b8 = data[8];
    map_hat(st.buttons, b8 & 0x0F);
    if (b8 & (1 << 4)) st.buttons |= X;      // Square
    if (b8 & (1 << 5)) st.buttons |= A;      // Cross
    if (b8 & (1 << 6)) st.buttons |= B;      // Circle
    if (b8 & (1 << 7)) st.buttons |= Y;      // Triangle

    uint8_t b9 = data[9];
    if (b9 & (1 << 0)) st.buttons |= LB;
    if (b9 & (1 << 1)) st.buttons |= RB;
    if (b9 & (1 << 2)) st.buttons |= LT;
    if (b9 & (1 << 3)) st.buttons |= RT;
    if (b9 & (1 << 4)) st.buttons |= BACK;   // Create
    if (b9 & (1 << 5)) st.buttons |= START;  // Options
    if (b9 & (1 << 6)) st.buttons |= LSTICK;
    if (b9 & (1 << 7)) st.buttons |= RSTICK;

    uint8_t b10 = data[10];
    if (b10 & (1 << 0)) st.buttons |= GUIDE; // PS

    if (data[5] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= LT;
    if (data[6] > TRIGGER_DIGITAL_THRESHOLD) st.buttons |= RT;
    publish(st);
}

// ============================================================
// Generic HID — descriptor parser
// ============================================================
//
// Walks short HID items (long items are reserved and never appear in real
// gamepad descriptors). Records bit offsets for the axes / hat / first 16
// buttons of report ID 0 (or the first report ID seen).

static int read_signed(const uint8_t *p, int size) {
    if (size == 1) return (int8_t)p[0];
    if (size == 2) return (int16_t)(p[0] | (p[1] << 8));
    if (size == 4) return (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    return 0;
}

static uint32_t read_unsigned(const uint8_t *p, int size) {
    if (size == 1) return p[0];
    if (size == 2) return (uint32_t)(p[0] | (p[1] << 8));
    if (size == 4) return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    return 0;
}

static bool parse_hid_report_descriptor(const uint8_t *desc, int desc_len,
                                        HidLayout *out) {
    *out = {};
    out->bit_lx = out->bit_ly = out->bit_rx = out->bit_ry = -1;
    out->bit_hat = -1;
    out->bit_btn0 = -1;

    uint16_t usage_page = 0;
    uint32_t usages[16] = {};
    int usage_count = 0;
    int32_t logical_min = 0, logical_max = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t  report_id = 0;
    uint32_t bit_cursor = 0;
    int      i = 0;

    auto take_usage = [&]() -> uint32_t {
        if (usage_count == 0) return 0;
        uint32_t u = usages[0];
        for (int k = 1; k < usage_count; ++k) usages[k - 1] = usages[k];
        usage_count--;
        return u;
    };

    while (i < desc_len) {
        uint8_t prefix = desc[i++];
        if (prefix == 0xFE) {                 // long item — skip
            if (i >= desc_len) break;
            uint8_t data_len = desc[i++];
            i += 2 + data_len;
            continue;
        }
        uint8_t size = prefix & 0x03;
        if (size == 3) size = 4;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag  = (prefix >> 4) & 0x0F;
        if (i + size > desc_len) break;
        const uint8_t *data = desc + i;
        i += size;

        if (type == 0) {                       // Main
            if (tag == 0x8) {                  // Input
                uint8_t flags = (size > 0) ? data[0] : 0;
                bool is_const = (flags & 0x01) != 0;
                bool is_var   = (flags & 0x02) != 0;

                if (!is_const && usage_page == 0x09 && is_var) {
                    // Buttons: usage_min..usage_max as a packed bitmap
                    if (out->bit_btn0 < 0) {
                        out->bit_btn0 = (int16_t)bit_cursor;
                        out->n_btn = (report_count <= 16) ? (uint8_t)report_count : 16;
                    }
                }
                if (!is_const && usage_page == 0x01 && is_var) {
                    // Generic Desktop usages (X, Y, Z, Rx, Ry, Rz, Hat, ...)
                    uint32_t units_per_field = report_size;
                    for (uint32_t f = 0; f < report_count; ++f) {
                        uint32_t u = take_usage();
                        int16_t  bit_off = (int16_t)(bit_cursor + f * units_per_field);
                        switch (u) {
                        case 0x30: out->bit_lx  = bit_off; out->sz_lx  = (uint8_t)units_per_field;
                                   out->sgn_lx  = (logical_min < 0) ? 1 : 0; break;
                        case 0x31: out->bit_ly  = bit_off; out->sz_ly  = (uint8_t)units_per_field;
                                   out->sgn_ly  = (logical_min < 0) ? 1 : 0; break;
                        case 0x32: case 0x33: // Z, Rx — treat as RX
                                   if (out->bit_rx < 0) {
                                       out->bit_rx = bit_off;
                                       out->sz_rx  = (uint8_t)units_per_field;
                                       out->sgn_rx = (logical_min < 0) ? 1 : 0;
                                   } break;
                        case 0x34: case 0x35: // Ry, Rz — treat as RY
                                   if (out->bit_ry < 0) {
                                       out->bit_ry = bit_off;
                                       out->sz_ry  = (uint8_t)units_per_field;
                                       out->sgn_ry = (logical_min < 0) ? 1 : 0;
                                   } break;
                        case 0x39: out->bit_hat = bit_off;
                                   out->sz_hat = (uint8_t)units_per_field; break;
                        default: break;
                        }
                    }
                    if (out->logical_max_axis == 0) {
                        out->logical_min_axis = logical_min;
                        out->logical_max_axis = logical_max;
                    }
                }
                bit_cursor += report_size * report_count;
            } else if (tag == 0x9 || tag == 0xB) {  // Output / Feature
                bit_cursor += report_size * report_count;
            }
            usage_count = 0;
            // Collection / EndCollection don't advance the cursor.
        } else if (type == 1) {                // Global
            uint32_t uval = read_unsigned(data, size);
            int32_t  sval = read_signed(data, size);
            switch (tag) {
            case 0x0: usage_page = (uint16_t)uval; break;
            case 0x1: logical_min = sval; break;
            case 0x2: logical_max = sval; break;
            case 0x7: report_size = uval; break;
            case 0x8: report_id = (uint8_t)uval;
                      if (!out->has_report_id) {
                          out->report_id = report_id;
                          out->has_report_id = true;
                      }
                      break;
            case 0x9: report_count = uval; break;
            default: break;
            }
        } else if (type == 2) {                // Local
            if (tag == 0x0 || tag == 0x1 || tag == 0x2) {
                uint32_t uval = read_unsigned(data, size);
                if (usage_count < 16) usages[usage_count++] = uval;
            }
        }
    }
    out->total_bits = (uint16_t)bit_cursor;
    bool found_any =
        out->bit_lx >= 0 || out->bit_ly >= 0 || out->bit_btn0 >= 0;
    return found_any;
}

// Extract `bits` bits starting at bit `off` from the little-endian byte
// stream. Returns up to 32 bits, sign-extended if `is_signed`.
static int32_t extract_bits(const uint8_t *data, int data_len,
                            uint32_t off, uint8_t bits, bool is_signed) {
    if (bits == 0 || bits > 32) return 0;
    uint32_t byte_off = off / 8;
    uint8_t  shift    = off % 8;
    uint64_t acc = 0;
    int      shift_bits = 0;
    while (shift_bits < (int)(shift + bits)) {
        if ((int)byte_off + shift_bits / 8 >= data_len) break;
        acc |= ((uint64_t)data[byte_off + shift_bits / 8]) << shift_bits;
        shift_bits += 8;
    }
    uint64_t v = (acc >> shift) & (((uint64_t)1 << bits) - 1);
    if (is_signed && bits < 32 && (v & ((uint64_t)1 << (bits - 1)))) {
        v |= ~(((uint64_t)1 << bits) - 1);
    }
    return (int32_t)v;
}

static int16_t hid_axis_to_int16(int32_t raw, int32_t lmin, int32_t lmax,
                                 bool is_signed) {
    if (lmax <= lmin) lmax = lmin + 1;
    if (!is_signed && lmin >= 0) {
        // Unsigned axis: center is the midpoint.
        int32_t mid = (lmin + lmax) / 2;
        int32_t span = (lmax - lmin) / 2;
        if (span == 0) return 0;
        int32_t scaled = (raw - mid) * 32767 / span;
        if (scaled >  32767) scaled =  32767;
        if (scaled < -32768) scaled = -32768;
        return (int16_t)scaled;
    }
    // Signed: scale to full int16 range.
    int32_t span = (lmax > -lmin) ? lmax : -lmin;
    if (span == 0) return 0;
    int32_t scaled = raw * 32767 / span;
    if (scaled >  32767) scaled =  32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static void parse_hid_generic(const uint8_t *data, int len) {
    if (len <= 0) return;
    const HidLayout &L = s_hid_layout;

    const uint8_t *report = data;
    int report_len = len;
    if (L.has_report_id) {
        if (data[0] != L.report_id) return;
        report = data + 1;
        report_len = len - 1;
    }

    using namespace InputDev;
    State st = {};
    st.any_connected = true;

    auto get_axis = [&](int16_t bit, uint8_t sz, uint8_t sgn) -> int16_t {
        if (bit < 0 || sz == 0) return 0;
        int32_t raw = extract_bits(report, report_len, bit, sz, sgn);
        return hid_axis_to_int16(raw, L.logical_min_axis, L.logical_max_axis, sgn);
    };
    st.lx = get_axis(L.bit_lx, L.sz_lx, L.sgn_lx);
    st.ly = get_axis(L.bit_ly, L.sz_ly, L.sgn_ly);
    st.rx = get_axis(L.bit_rx, L.sz_rx, L.sgn_rx);
    st.ry = get_axis(L.bit_ry, L.sz_ry, L.sgn_ry);

    if (L.bit_hat >= 0 && L.sz_hat > 0) {
        uint32_t hat = (uint32_t)extract_bits(report, report_len,
                                              L.bit_hat, L.sz_hat, false);
        map_hat(st.buttons, (uint8_t)(hat & 0x0F));
    }

    if (L.bit_btn0 >= 0 && L.n_btn > 0) {
        static const uint32_t btn_map[16] = {
            A, B, X, Y, LB, RB, BACK, START, GUIDE,
            LSTICK, RSTICK, LT, RT, DPAD_UP, DPAD_DOWN, DPAD_LEFT,
        };
        for (uint8_t i = 0; i < L.n_btn && i < 16; ++i) {
            if (extract_bits(report, report_len,
                             L.bit_btn0 + i, 1, false)) {
                st.buttons |= btn_map[i];
            }
        }
    }
    publish(st);
}

// ============================================================
// IN-transfer callback — dispatch to the right parser
// ============================================================

static int s_xfer_dbg = 0;
static int s_xfer_ok = 0, s_xfer_fail = 0;

static void in_xfer_cb(usb_transfer_t *xfer) {
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED) s_xfer_ok++;
    else s_xfer_fail++;
    if ((s_xfer_ok + s_xfer_fail) % 200 == 1) {
        ESP_LOGI(TAG, "xfer ok=%d fail=%d status=%d bytes=%d",
                 s_xfer_ok, s_xfer_fail, xfer->status, xfer->actual_num_bytes);
    }

    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
        uint8_t *d = xfer->data_buffer;
        int n = xfer->actual_num_bytes;
        if (s_xfer_dbg++ % 500 == 0) {
            ESP_LOGI(TAG, "raw[%d]: %02X %02X %02X %02X %02X %02X %02X %02X",
                     n, d[0], n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0,
                     n > 4 ? d[4] : 0, n > 5 ? d[5] : 0, n > 6 ? d[6] : 0, n > 7 ? d[7] : 0);
        }
        switch (s_dev_type) {
        case DEV_XBOX_360:    parse_xbox360(d, n); break;
        case DEV_XBOX_ONE:    parse_xboxone(d, n); break;
        case DEV_DS4:         parse_ds4(d, n); break;
        case DEV_DUALSENSE:   parse_dualsense(d, n); break;
        case DEV_HID_GENERIC: parse_hid_generic(d, n); break;
        default: break;
        }
    } else if (xfer->status == USB_TRANSFER_STATUS_STALL) {
        usb_host_endpoint_clear(s_device, s_ep_in);
    }

    if (s_device_connected) {
        esp_err_t err = usb_host_transfer_submit(s_in_xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Re-submit failed: %s", esp_err_to_name(err));
        }
    }
}

// ============================================================
// HID Report Descriptor fetch (control transfer GET_DESCRIPTOR)
// ============================================================

static bool fetch_hid_report_descriptor(uint8_t iface, uint16_t length,
                                        uint8_t *buf, int buf_size,
                                        int *out_len) {
    usb_transfer_t *xfer = nullptr;
    int total = (int)length + 8;            // 8-byte SETUP
    if (total > buf_size + 8) length = (uint16_t)(buf_size);
    usb_host_transfer_alloc((size_t)length + 8, 0, &xfer);
    if (!xfer) return false;

    usb_setup_packet_t setup = {};
    setup.bmRequestType = 0x81;             // dev → host, std, interface
    setup.bRequest = 0x06;                  // GET_DESCRIPTOR
    setup.wValue   = 0x2200;                // HID Report descriptor
    setup.wIndex   = iface;
    setup.wLength  = length;
    memcpy(xfer->data_buffer, &setup, sizeof(setup));

    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xfer->bEndpointAddress = 0;
    xfer->device_handle = s_device;
    xfer->num_bytes = length + 8;
    xfer->callback = [](usb_transfer_t *t) {
        auto *sem = (SemaphoreHandle_t)t->context;
        xSemaphoreGive(sem);
    };
    xfer->context = done;
    esp_err_t r = usb_host_transfer_submit_control(s_client, xfer);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "GET_DESC submit fail: %s", esp_err_to_name(r));
        vSemaphoreDelete(done);
        usb_host_transfer_free(xfer);
        return false;
    }
    if (xSemaphoreTake(done, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "GET_DESC timeout");
        vSemaphoreDelete(done);
        usb_host_transfer_free(xfer);
        return false;
    }
    vSemaphoreDelete(done);

    int got = xfer->actual_num_bytes - 8;
    if (got <= 0 || xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        usb_host_transfer_free(xfer);
        return false;
    }
    int copy = got > buf_size ? buf_size : got;
    memcpy(buf, xfer->data_buffer + 8, copy);
    *out_len = copy;
    usb_host_transfer_free(xfer);
    return true;
}

// ============================================================
// Device classification + open
// ============================================================

struct OpenPlan {
    DevType  type;
    int      iface;
    uint8_t  ep_in;
    int      ep_in_mps;
    uint16_t hid_report_descriptor_len; // 0 if N/A
};

static bool classify_device(uint16_t vid, uint16_t pid,
                            const usb_config_desc_t *config_desc,
                            OpenPlan *plan) {
    *plan = { DEV_NONE, -1, 0, 0, 0 };
    const uint8_t *p = (const uint8_t *)config_desc;
    int offset = 0;
    int cur_iface = -1;
    DevType cur_type = DEV_NONE;
    uint16_t hid_rd_len = 0;

    while (offset < config_desc->wTotalLength) {
        const usb_standard_desc_t *d = (const usb_standard_desc_t *)(p + offset);

        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *iface = (const usb_intf_desc_t *)d;
            ESP_LOGI(TAG, "  iface %d: class=0x%02X sub=0x%02X proto=0x%02X",
                     iface->bInterfaceNumber, iface->bInterfaceClass,
                     iface->bInterfaceSubClass, iface->bInterfaceProtocol);

            cur_iface = -1;
            cur_type  = DEV_NONE;
            hid_rd_len = 0;
            if (plan->type != DEV_NONE) {
                // Already locked onto an interface; skip the rest.
                offset += d->bLength;
                continue;
            }

            // Sony PlayStation pads — match by VID/PID, then accept the
            // first HID interface.
            if (vid == VID_SONY) {
                if (iface->bInterfaceClass == 0x03) {
                    if (pid == PID_DS4_V1 || pid == PID_DS4_V2) {
                        cur_iface = iface->bInterfaceNumber;
                        cur_type  = DEV_DS4;
                    } else if (pid == PID_DUALSENSE) {
                        cur_iface = iface->bInterfaceNumber;
                        cur_type  = DEV_DUALSENSE;
                    }
                }
            }
            // Xbox 360 — vendor class 0xFF, subclass 0x5D, protocol 0x01
            if (cur_type == DEV_NONE
                && iface->bInterfaceClass == 0xFF
                && iface->bInterfaceSubClass == 0x5D
                && iface->bInterfaceProtocol == 0x01) {
                cur_iface = iface->bInterfaceNumber;
                cur_type  = DEV_XBOX_360;
            }
            // Xbox One / Series — vendor class 0xFF, subclass 0x47
            if (cur_type == DEV_NONE
                && iface->bInterfaceClass == 0xFF
                && iface->bInterfaceSubClass == 0x47) {
                cur_iface = iface->bInterfaceNumber;
                cur_type  = DEV_XBOX_ONE;
            }
            // Generic HID gamepad / joystick — class 0x03, sub 0, proto 0
            if (cur_type == DEV_NONE
                && iface->bInterfaceClass == 0x03
                && iface->bInterfaceSubClass == 0x00
                && iface->bInterfaceProtocol == 0x00) {
                cur_iface = iface->bInterfaceNumber;
                cur_type  = DEV_HID_GENERIC;
            }
        } else if (d->bDescriptorType == 0x21 && cur_type != DEV_NONE) {
            // HID class descriptor — locate the report descriptor length.
            const uint8_t *raw = (const uint8_t *)d;
            if (d->bLength >= 9) {
                hid_rd_len = (uint16_t)(raw[7] | (raw[8] << 8));
            }
        } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT
                   && cur_type != DEV_NONE && plan->type == DEV_NONE) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if (ep->bEndpointAddress & 0x80) {
                plan->type      = cur_type;
                plan->iface     = cur_iface;
                plan->ep_in     = ep->bEndpointAddress;
                plan->ep_in_mps = ep->wMaxPacketSize;
                plan->hid_report_descriptor_len = hid_rd_len;
                ESP_LOGI(TAG, "Selected %s on iface %d, EP=0x%02X MPS=%d rd_len=%d",
                         dev_type_name(cur_type),
                         cur_iface, plan->ep_in, plan->ep_in_mps, hid_rd_len);
            }
        }
        offset += d->bLength;
    }
    return plan->type != DEV_NONE;
}

static bool try_open_device(uint8_t dev_addr) {
    if (usb_host_device_open(s_client, dev_addr, &s_device) != ESP_OK) return false;

    const usb_device_desc_t *desc;
    usb_host_get_device_descriptor(s_device, &desc);
    ESP_LOGI(TAG, "USB device: VID=0x%04X PID=0x%04X class=%d",
             desc->idVendor, desc->idProduct, desc->bDeviceClass);
    snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: VID=%04X PID=%04X",
             desc->idVendor, desc->idProduct);

    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(s_device, &config_desc);

    OpenPlan plan;
    if (!classify_device(desc->idVendor, desc->idProduct, config_desc, &plan)) {
        ESP_LOGW(TAG, "No supported interface on this device");
        snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: unsupported");
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        return false;
    }

    if (usb_host_interface_claim(s_client, s_device, plan.iface, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Interface claim failed");
        usb_host_device_close(s_client, s_device);
        s_device = nullptr;
        return false;
    }

    s_dev_type    = plan.type;
    s_ep_in       = plan.ep_in;
    s_ep_in_mps   = plan.ep_in_mps;

    if (plan.type == DEV_HID_GENERIC && plan.hid_report_descriptor_len > 0) {
        uint8_t rd[512];
        int rd_len = 0;
        if (fetch_hid_report_descriptor((uint8_t)plan.iface,
                                        plan.hid_report_descriptor_len,
                                        rd, sizeof(rd), &rd_len)) {
            ESP_LOGI(TAG, "HID report descriptor: %d bytes", rd_len);
            if (!parse_hid_report_descriptor(rd, rd_len, &s_hid_layout)) {
                ESP_LOGW(TAG, "HID descriptor parse: no recognized fields");
                usb_host_interface_release(s_client, s_device, plan.iface);
                usb_host_device_close(s_client, s_device);
                s_device = nullptr;
                snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: HID layout n/a");
                return false;
            }
            ESP_LOGI(TAG, "HID layout: LX=%d LY=%d RX=%d RY=%d hat=%d btn0=%d nbtn=%d",
                     s_hid_layout.bit_lx, s_hid_layout.bit_ly,
                     s_hid_layout.bit_rx, s_hid_layout.bit_ry,
                     s_hid_layout.bit_hat, s_hid_layout.bit_btn0,
                     s_hid_layout.n_btn);
        } else {
            ESP_LOGW(TAG, "Could not fetch HID report descriptor");
            usb_host_interface_release(s_client, s_device, plan.iface);
            usb_host_device_close(s_client, s_device);
            s_device = nullptr;
            return false;
        }
    }

    usb_host_transfer_alloc(s_ep_in_mps, 0, &s_in_xfer);
    s_in_xfer->device_handle    = s_device;
    s_in_xfer->bEndpointAddress = s_ep_in;
    s_in_xfer->callback         = in_xfer_cb;
    s_in_xfer->num_bytes        = s_ep_in_mps;

    s_device_connected = true;
    snprintf(s_debug_msg, sizeof(s_debug_msg), "GP: %s connected!",
             dev_type_name(plan.type));

    if (plan.type == DEV_XBOX_ONE) xboxone_send_init();

    esp_err_t sub_ret = usb_host_transfer_submit(s_in_xfer);
    ESP_LOGI(TAG, "Reading started, submit=%s", esp_err_to_name(sub_ret));
    return true;
}

// ============================================================
// Client / host event loops
// ============================================================

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New USB device: addr=%d", msg->new_dev.address);
        if (!s_device_connected) try_open_device(msg->new_dev.address);
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
        s_dev_type = DEV_NONE;
        s_hid_layout = {};
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state = {};
        xSemaphoreGive(s_state_mutex);
        break;
    }
}

static void usb_host_task(void *arg) {
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_client_task(void *arg) {
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

// ============================================================
// InputDev::Source
// ============================================================

static bool usb_connected() { return s_device_connected; }

static void usb_poll(InputDev::State *out) {
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_state_mutex);
}

static const char *usb_status_msg() { return s_debug_msg; }

static const InputDev::Source s_usb_source = {
    "usb-gamepad",
    usb_connected,
    usb_poll,
    usb_status_msg,
};

// ============================================================
// Public init
// ============================================================

void input_usb_init() {
    s_state_mutex = xSemaphoreCreateMutex();

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

    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 4096, nullptr, 5, nullptr, 0);

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

    xTaskCreatePinnedToCore(usb_client_task, "usb_gpad", 4096, nullptr, 5, nullptr, 0);

    InputDev::register_source(&s_usb_source);
    ESP_LOGI(TAG, "USB gamepad source registered (X360/XOne/DS4/DualSense/HID)");
}
