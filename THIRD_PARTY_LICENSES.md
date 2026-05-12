# Third-Party Licenses

`sb3tab` is licensed under **LGPL-3.0-or-later** (see [LICENSE](./LICENSE) for
the GNU Lesser General Public License v3 and [COPYING](./COPYING) for the GNU
General Public License v3, which LGPL incorporates by reference).
It bundles or links against the following third-party software, each under its
own license. Attribution and license notices are preserved here in addition to
any in-source headers.

---

## Bundled source / vendored components

### Scratch Everywhere!
- Path: `components/scratch_core/`
- Upstream: https://github.com/NateXS/Scratch-Everywhere
- License: **LGPL-3.0-or-later** (see `components/scratch_core/LICENSE`)
- Note: the `sb3` runtime, block opcode dispatch, asset loading, audio mixing,
  software renderer, headless input, and most of the menu/UI graphics under
  `components/scratch_core/gfx/` come from this project. The included splash
  audio `mm_splash.ogg` and `splashText.txt` credit NateXS and Grady Link.

### nlohmann/json
- Path: `components/scratch_core/deps/nlohmann/`
- Upstream: https://github.com/nlohmann/json
- License: **MIT**

### miniz
- Path: `components/scratch_core/deps/miniz/`
- Upstream: https://github.com/richgel999/miniz
- License: **MIT**

### minimp3
- Path: `components/scratch_core/deps/minimp3/`
- Upstream: https://github.com/lieff/minimp3
- License: **CC0-1.0** (public domain)

### stb_truetype, stb_image, stb_image_write
- Paths: `main/stb_truetype.h`, `main/stb_image.h`, `main/stb_image_write.h`,
  `components/scratch_core/deps/stb_truetype.h`
- Upstream: https://github.com/nothings/stb
- License: **MIT or Public Domain (dual)** — at user's choice

### nanosvg / nanosvgrast
- Paths: `main/nanosvg.h`, `main/nanosvgrast.h`
- Upstream: https://github.com/memononen/nanosvg
- License: **zlib**

### nonstd (span-lite etc.)
- Path: `components/scratch_core/deps/nonstd/`
- Upstream: https://github.com/martinmoene
- License: **BSL-1.0** (Boost Software License)

### ryujs
- Path: `components/scratch_core/deps/ryujs/`
- Upstream: https://github.com/ulfjack/ryu
- License: **Apache-2.0 or Boost-1.0 (dual)**

### esp_lcd_st7123 driver
- Paths: `main/esp_lcd_st7123.c`, `main/esp_lcd_st7123.h`
- Adapted from M5Stack BSP / Espressif example drivers
- License: **Apache-2.0**

---

## ESP-IDF managed components

All under `managed_components/`. Espressif components are **Apache-2.0** unless
noted otherwise. See each component's own LICENSE file for canonical text.

| Component | License |
|---|---|
| `espressif/cmake_utilities` | Apache-2.0 |
| `espressif/eppp_link` | Apache-2.0 |
| `espressif/esp_cam_sensor` | Apache-2.0 |
| `espressif/esp_h264` | Apache-2.0 |
| `espressif/esp_hosted` (v1.4.7) | Apache-2.0 |
| `espressif/esp_ipa` | Apache-2.0 |
| `espressif/esp_lcd_ili9341` | Apache-2.0 |
| `espressif/esp_lcd_touch` | Apache-2.0 |
| `espressif/esp_lcd_touch_st7123` | Apache-2.0 |
| `espressif/esp_lvgl_port` | Apache-2.0 |
| `espressif/esp_sccb_intf` | Apache-2.0 |
| `espressif/esp_serial_slave_link` | Apache-2.0 |
| `espressif/esp_video` | Apache-2.0 |
| `espressif/esp_wifi_remote` | Apache-2.0 |
| `espressif/quirc` | **ISC** (upstream by Daniel Beer) |
| `espressif/usb_host_uvc` | Apache-2.0 |
| `espressif/wifi_remote_over_eppp` | Apache-2.0 |
| `lvgl/lvgl` | **MIT** |

ESP-IDF itself (linked at build time) is **Apache-2.0**.

---

## Fonts

| Font | Path | License |
|---|---|---|
| Noto Sans JP (subset) | `components/scratch_core/gfx/ingame/fonts/NotoSansJP-Medium-subset.ttf` | **SIL Open Font License 1.1** |
| Noto Sans Medium | `components/scratch_core/gfx/ingame/fonts/NotoSans-Medium.ttf` | **SIL Open Font License 1.1** |
| Noto Serif Regular | `components/scratch_core/gfx/ingame/fonts/NotoSerif-Regular.ttf` | **SIL Open Font License 1.1** |
| Grand9KPixel | `components/scratch_core/gfx/ingame/fonts/Grand9KPixel.ttf` | **CC0-1.0** |
| Griffy Regular | `components/scratch_core/gfx/ingame/fonts/Griffy-Regular.ttf` | **SIL Open Font License 1.1** |
| Handlee Regular | `components/scratch_core/gfx/ingame/fonts/Handlee-Regular.ttf` | **SIL Open Font License 1.1** |
| Knewave Regular | `components/scratch_core/gfx/ingame/fonts/Knewave-Regular.ttf` | **SIL Open Font License 1.1** |
| Ubuntu Bold | `components/scratch_core/gfx/menu/Ubuntu-Bold.ttf` | **Ubuntu Font License 1.0** |

---

## Project assets

The `sb3tab` wordmark logo (`main/assets/logo.png`) and the icons under
`main/assets/` (icon_play.png, icon_new.png, icon_settings.png) are original to
this project and released under the same LGPL-3.0-or-later as the rest of the
source. The accompanying sound effects (`cancel.wav`, `cursor.wav`,
`select.wav`) are likewise project-original.

---

## Trademarks

**Scratch** is a trademark of the Scratch Foundation. `sb3tab` is an
independent open-source project that is **not affiliated with, endorsed by, or
sponsored by** the Scratch Foundation or MIT. `sb3tab` plays back projects
saved in Scratch's `.sb3` file format under nominative fair use. The name
"Scratch" is intentionally not used as part of this project's product name.

**ESP32**, **ESP-IDF**, and related marks are trademarks of Espressif Systems.

**M5Stack**, **M5Tab5**, **M5Stamp** are trademarks of M5Stack Technology
Co., Ltd.

All other trademarks are the property of their respective owners.
