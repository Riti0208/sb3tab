# ScratchESP

[**日本語版 README はこちら**](README.ja.md)

Run [Scratch](https://scratch.mit.edu/) projects (`.sb3`) natively on ESP32 microcontrollers — no browser, no PC, no internet required.

Built on top of [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere), a cross-platform Scratch runtime.

## Features

| Feature | Status |
|---|---|
| Sprite rendering (PNG/SVG) | ✅ |
| Stage backdrop | ✅ |
| Speech bubbles (say/think) | ✅ Japanese text supported |
| Pen drawing | ✅ Lines, dots, stamp, clear |
| Variable/list monitors | ✅ |
| Brightness effect | ✅ |
| Ghost effect | ✅ |
| Sound playback (WAV) | ✅ |
| Sound pitch/pan effects | ✅ |
| Clones | ✅ |
| Motion/control/operator/data blocks | ✅ |

### Not yet implemented
- Graphic effects: COLOR, FISHEYE, WHIRL, PIXELATE, MOSAIC (not in upstream ScratchEverywhere)
- Color touching detection (not in upstream)
- MP3/Ogg audio (WAV PCM only)
- Input (keyboard/mouse/touch — needs hardware)
- Text-to-Speech (needs WiFi)

## Hardware

### Tested
- **ESP32-P4** (M5Stamp P4) + SPI LCD (ILI9341, 320x240) — 10-13 fps
- **ESP32-S3** (XIAO ESP32-S3 Sense) + SPI LCD — fully working

### Planned
- **M5Stack Tab5** (ESP32-P4 + 5" 1280x720 MIPI DSI) — targeting 30+ fps

### Audio (optional)
- MAX98357A I2S DAC module
- I2S pins: BCLK=GPIO5, LRCLK=GPIO6, DOUT=GPIO8

## Build

### Requirements
- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/)
- ESP32-P4 target (also works with ESP32-S3 with minor changes)

### Steps

```bash
# Set target
idf.py set-target esp32p4

# Build
idf.py build

# Flash
idf.py -p /dev/tty.usbmodem1101 flash
```

> **Note:** After `set-target`, you may need to restore `sdkconfig` settings:
> `rm sdkconfig && idf.py reconfigure`

## Usage

### 1. Send a Scratch project

```bash
# Download a project from scratch.mit.edu
python3 -u tools/send_sb3.py <project.sb3 or URL> /dev/tty.usbmodem1101
```

The tool extracts the `.sb3` on the host (since zip decompression crashes on P4's RISC-V) and sends `project.json` + assets individually over USB serial.

### 2. Watch it run

The project starts executing immediately on the ESP32. Sprites are rendered at 480x360 internally and scaled to the LCD resolution.

## Architecture

```
┌──────────────────────────────────┐
│  Host PC                         │
│  tools/send_sb3.py               │
│  (extract .sb3, send via USB)    │
└──────────┬───────────────────────┘
           │ USB Serial (LL API)
┌──────────▼───────────────────────┐
│  ESP32-P4                        │
│                                  │
│  ┌─────────────────────────────┐ │
│  │ scratch_core (component)    │ │
│  │  - Runtime (block executor) │ │
│  │  - Parser (JSON → sprites)  │ │
│  │  - Headless renderer        │ │
│  │  - SpeechManager (TTF)      │ │
│  │  - Audio (I2S + WAV)        │ │
│  └─────────────┬───────────────┘ │
│                │                 │
│  ┌─────────────▼───────────────┐ │
│  │ main/                       │ │
│  │  - SWRenderer (480x360)     │ │
│  │  - LCD driver (SPI/DSI)     │ │
│  │  - USB protocol handler     │ │
│  └─────────────────────────────┘ │
└──────────────────────────────────┘
```

### Render pipeline
1. Clear framebuffer (white)
2. Draw stage backdrop
3. Composite pen layer (RGBA8888)
4. Draw sprites with rotation, scaling, alpha
5. Draw speech bubbles (stb_truetype + NotoSansJP)
6. Draw variable/list monitors
7. Scale & push to LCD

### Key technical decisions
- **stb_truetype** for font rendering (NotoSansJP subset, 331KB, embedded in flash)
- **nanosvg** for SVG rasterization
- **USB LL API** instead of USB driver (avoids VFS conflicts)
- **Host-side .sb3 extraction** (miniz crashes on P4 RISC-V)
- **Pen callbacks** bridge scratch_core component → main SWRenderer

## Credits

- [ScratchEverywhere](https://github.com/ScratchEverywhere/ScratchEverywhere) — the Scratch runtime this project builds upon
- [Scratch](https://scratch.mit.edu/) by MIT Media Lab
- [stb libraries](https://github.com/nothings/stb) — stb_truetype, stb_image
- [nanosvg](https://github.com/memononen/nanosvg) — SVG parser and rasterizer
- [NotoSansJP](https://fonts.google.com/noto/specimen/Noto+Sans+JP) — Google's Japanese font

## License

This project uses ScratchEverywhere which is licensed under GPL-3.0.
