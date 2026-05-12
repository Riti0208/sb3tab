#pragma once

// USB Host gamepad backend for the InputDev layer.
//
// Supports:
//   - Xbox 360 wired pads (vendor subclass 0x5D)
//   - Xbox One / Series wired pads (vendor subclass 0x47)
//   - Sony DualShock 4 (PID 0x05C4 / 0x09CC)
//   - Sony DualSense (PID 0x0CE6)
//   - Generic USB HID gamepads / joysticks
//
// Call once at startup, after DSI / IO expander init has powered the USB-A
// port. `usb_host_install()` is invoked from here and must not be called
// elsewhere in the firmware.
void input_usb_init();
