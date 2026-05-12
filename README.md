# sb3tab

<p align="center">
  <img src="main/assets/logo.png" alt="sb3tab logo" width="420">
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: LGPL-3.0-or-later" src="https://img.shields.io/badge/License-LGPL--3.0--or--later-blue.svg"></a>
  <a href="https://github.com/Riti0208/sb3tab/actions/workflows/build.yml"><img alt="Build" src="https://github.com/Riti0208/sb3tab/actions/workflows/build.yml/badge.svg"></a>
  <img alt="Target" src="https://img.shields.io/badge/target-ESP32--P4-orange">
</p>

[**日本語版 README はこちら**](README.ja.md)

<!-- TODO: replace with a real demo GIF / video showing QR scan → download → run -->
<!--
<p align="center">
  <img src="docs/demo.gif" alt="sb3tab running a Scratch project on M5Stack Tab5" width="540">
</p>
-->

Run Scratch projects (`.sb3` file format) natively on ESP32 microcontrollers — no browser, no PC, no internet required at runtime.

Built on top of [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere), a cross-platform `.sb3` runtime.

> `sb3tab` is an independent open-source project and is **not affiliated with, endorsed by, or sponsored by** the Scratch Foundation or MIT.

## Highlights

- **34 fps average** on M5Stack Tab5 (5" 720x1280 MIPI DSI), measured on an 11-sprite + 10-clone project
- **Scan a QR code → download from scratch.mit.edu → run** — fully standalone after first WiFi setup
- **Onboard sound** via the Tab5 ES8388 codec (WAV + MP3, pitch/pan effects)
- **Touch + USB gamepad input** (Xbox 360, Xbox One/Series, DualShock 4, DualSense, generic HID) with auto-mapping from project key bindings
- **Japanese text** in speech bubbles and monitors (NotoSansJP, JIS X 0208 Level 1)
- Boot splash, main menu, settings UI, on-device QR scanner, and SD card persistence

## Supported features

| Feature | Status |
|---|---|
| Sprite rendering (PNG/SVG, including `<text>`/`<tspan>`) | ✅ |
| Stage backdrop | ✅ |
| Speech bubbles (say/think) | ✅ Japanese supported |
| Pen drawing | ✅ Lines, dots, stamp, clear |
| Variable / list monitors | ✅ |
| Brightness, ghost effects | ✅ |
| Color-touching / touching-color sensing | ✅ Opt-in |
| Sound playback | ✅ WAV + MP3, pitch/pan |
| Clones | ✅ |
| Motion / control / operator / data blocks | ✅ |
| Keyboard input via touch on-screen / GPIO buttons / USB gamepad | ✅ |
| Rotation styles (LEFT_RIGHT, NONE, ALL_AROUND) | ✅ |

### USB gamepad compatibility

Plug a wired USB gamepad into the Tab5's USB-A port.

**Bench-verified:**

- Xbox 360 wired
- Sony DualShock 4 (PID 0x05C4 / 0x09CC)

**Coded but not yet hardware-verified:** Sony DualSense (PID 0x0CE6), Xbox
One / Series wired (partial GIP handshake), and generic USB HID gamepads
(interface class 0x03 / sub 0x00 / proto 0x00). The HID Report Descriptor
is parsed at open time, so most plain "DirectInput / HID" pads should
enumerate.

### Not yet implemented (runtime)

- Graphic effects: COLOR, FISHEYE, WHIRL, PIXELATE, MOSAIC (not in upstream ScratchEverywhere)
- Ogg audio
- Text-to-Speech and translation blocks

## Hardware

### Primary target — M5Stack Tab5

| Subsystem | Detail |
|---|---|
| MCU | ESP32-P4 @ 360 MHz, 32 MB PSRAM, 16 MB flash |
| Display | 5" 720x1280 MIPI DSI (ST7123), capacitive touch |
| WiFi | ESP32-C6 over SDIO via [esp_hosted](https://github.com/espressif/esp-hosted) v1.4.7 |
| Audio | ES8388 I²S codec + onboard speaker |
| Camera | SC202CS MIPI CSI (used for QR scanning) |
| Storage | SD card (FAT) for projects + WiFi credentials |
| Input | Touch, GPIO buttons, USB-A host (Xbox 360 / Xbox One / DS4 / DualSense / HID gamepads) |

The slave firmware for the onboard ESP32-C6 lives in [`slave/`](slave/) (vendored from `esp_hosted`).

## Build

### Requirements

- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/) (v5.5.x has a known DSI regression)
- Python 3 with `qrcode` and `requests` for the host-side tools

### Steps

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/tty.usbmodem1101 flash monitor
```

> After `set-target`, `sdkconfig` is regenerated from `sdkconfig.defaults`.
> If you hit unexpected config drift: `rm sdkconfig && idf.py reconfigure`.

## Usage

> **Before first run:** insert a microSD card into the Tab5. sb3tab needs
> writable storage for WiFi credentials (`/sd/wifi.txt`), settings
> (`/sd/brightness.txt`, `/sd/volume.txt`, `/sd/lang.txt`), and downloaded
> projects (`/sd/games/<id>/`). FAT32 or exFAT, formatted by the Tab5 or
> a host — sb3tab does not auto-format.

Generate the QR codes with the
[Scratcher QR Chrome extension](tools/chrome-extension/README.md). The
extension auto-detects the Scratch project page you currently have open,
and also handles WiFi QR generation.

Then on the device, open the QR scanner from the main menu:

- WiFi QR → credentials saved to `/sd/wifi.txt`
- Project QR → project + assets streamed from the Scratch CDN to
  `/sd/games/<id>/`, then run

## Architecture

```
┌──────────────────────────────────────────────┐
│  Source                                      │
│  • QR (camera) — scratch.mit.edu over WiFi   │
│  • SD card    — previously downloaded games  │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│  ESP32-P4 (Tab5)                             │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │ scratch_core (in-tree component)       │  │
│  │  • Block runtime + parser              │  │
│  │  • Headless renderer + speech manager  │  │
│  │  • Audio mixer (WAV / MP3)             │  │
│  └────────────────┬───────────────────────┘  │
│                   │                          │
│  ┌────────────────▼───────────────────────┐  │
│  │ main/                                  │  │
│  │  • SWRenderer (480x360, dual-core)     │  │
│  │  • DSI panel + PPA scale/rotate        │  │
│  │  • UI menu, settings, modals           │  │
│  │  • WiFi (esp_hosted), SD, time sync    │  │
│  │  • Camera + quirc QR decode            │  │
│  │  • Touch / GPIO / USB gamepad input    │  │
│  │  • ES8388 audio                        │  │
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

### Render pipeline

1. **Core 0**: clear next frame’s back buffer (pipelined with previous frame’s step)
2. **Core 1**: run Scratch step (block dispatch, motion, sensing)
3. Both cores: composite sprites in parallel (even / odd split)
4. Pen layer overlay (RGBA8888)
5. Speech bubbles + variable monitors
6. PPA SRM: scale 2× + rotate 270° CCW + RGB888 → RGB565 in one HW pass
7. DSI swap (double-buffered, no `memcpy`)

### Key technical decisions

- **PPA (Pixel Processing Accelerator)** for the final scale / rotate / pixel-format conversion
- **Dual-core dispatch with pipelined clear** — overlap step and clear to hide PSRAM bandwidth
- **Fixed-point 16.16 inner loop** in `blitRGBA` and an internal-SRAM costume cache (≤ 64 KB)
- **Host-side `.sb3` extraction** — miniz inflate is unstable on P4 RISC-V
- **USB Serial/JTAG LL API** instead of the VFS driver to avoid console conflicts
- **`esp_hosted` v1.4.7** for ESP32-C6 WiFi (v2.x crashes during FreeRTOS idle alloc)
- **stb_truetype + NotoSansJP subset** (JIS X 0208 Level 1, embedded in flash)

## Repository layout

```
main/                Tab5 firmware (display, input, audio, UI, networking)
components/
  scratch_core/      Scratch runtime (vendored ScratchEverywhere fork)
slave/               ESP32-C6 esp_hosted slave firmware
tools/
  chrome-extension/  Browser extension for QR generation
  gen_qr.py          CLI QR generator (developer convenience)
  subset_font.py     NotoSansJP subset builder
```

## Credits

- [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere) — the Scratch runtime this project builds upon
- [Scratch](https://scratch.mit.edu/) by MIT Media Lab
- [esp_hosted](https://github.com/espressif/esp-hosted) — ESP32-C6 WiFi co-processor stack
- [quirc](https://github.com/dlbeer/quirc) — QR code decoder
- [stb libraries](https://github.com/nothings/stb) — stb_truetype, stb_image
- [nanosvg](https://github.com/memononen/nanosvg) — SVG parser and rasterizer
- [minimp3](https://github.com/lieff/minimp3) — MP3 decoder
- [NotoSansJP](https://fonts.google.com/noto/specimen/Noto+Sans+JP) — Google's Japanese font

## License

`sb3tab` is licensed under **LGPL-3.0-or-later**. See [LICENSE](./LICENSE) for
the full GNU LGPL v3 / GPL v3 texts, and [THIRD_PARTY_LICENSES.md](./THIRD_PARTY_LICENSES.md)
for a complete inventory of bundled third-party software, fonts, and
attribution notices.

### Trademarks

**Scratch** is a trademark of the Scratch Foundation. This project plays back
`.sb3` files under nominative fair use and does not use "Scratch" as part of
its product name. **ESP32** / **ESP-IDF** are trademarks of Espressif Systems.
**M5Stack** / **M5Tab5** / **M5Stamp** are trademarks of M5Stack Technology
Co., Ltd. All other trademarks belong to their respective owners.
