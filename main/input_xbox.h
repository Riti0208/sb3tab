#pragma once

// USB Host Xbox controller backend for the InputDev layer.
// Supports Xbox 360 (vendor class 0x5D) and Xbox One/Series (vendor class 0x47).
// Call once at startup (after DSI/IO expander init for USB 5V power).
void input_xbox_init();
