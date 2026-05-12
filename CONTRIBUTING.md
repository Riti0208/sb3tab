# Contributing to sb3tab

Thanks for taking the time to contribute! sb3tab is a hobby project that grew
out of a single-developer experiment, so the contribution flow is intentionally
lightweight — be pragmatic, optimize for shipping fixes, and don't sweat
formalities.

## Ground rules

- **License**: by submitting a pull request you agree that your contribution is
  released under the project's license (**LGPL-3.0-or-later**, see
  [LICENSE](./LICENSE)). No CLA is required.
- **Be kind**: this project follows the
  [Contributor Covenant](./CODE_OF_CONDUCT.md).
- **Trademarks**: sb3tab is not affiliated with the Scratch Foundation. Don't
  add "Scratch" to product names, branding, or marketing materials.

## Development environment

Primary target hardware is **M5Stack Tab5** (ESP32-P4 + MIPI DSI). Secondary
targets are M5Stamp P4 + SPI ILI9341 and XIAO ESP32-S3 Sense.

Toolchain:

- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32p4/get-started/)
  (v5.5.x has a known DSI regression — do **not** upgrade without verifying
  your changes still build on v5.4.x)
- Python 3 with `qrcode`, `requests`, `Pillow` for host-side tools
- `magick` (ImageMagick) for asset processing helpers

Build:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

If the build picks up stale config after `set-target`, regenerate with:

```bash
rm sdkconfig && idf.py reconfigure
```

## Code style

- C / C++: follow the surrounding file. ESP-IDF-flavored C is fine in `.c`
  files; modern C++17 in `.cpp`. Prefer `static` for file-local helpers.
- Header guards: `#pragma once`.
- Comments explain **why**, not what. Identifier-level naming should carry the
  what.
- No trailing whitespace, LF line endings.
- Python: PEP 8-ish, no enforced formatter.

## Pull requests

1. Fork and branch from `main`.
2. Keep each PR focused on one logical change. Refactors and feature work in
   separate PRs.
3. Write commit messages in the form `scope: imperative summary` (lowercase
   scope), matching the existing history. Examples:
   - `runtime: fix Makey Makey block opcode typo`
   - `ui: gate LVGL indevs while game owns the screen`
4. In the PR description, include:
   - what changed and **why**
   - which hardware/firmware combinations you tested
   - any side effects on frame rate / power / memory if relevant
5. If you touch a third-party dependency or add one, update
   [THIRD_PARTY_LICENSES.md](./THIRD_PARTY_LICENSES.md).

## Testing

There is no host-side unit test suite yet. The expected verification path is:

- Build cleanly with `idf.py build` (CI also runs this).
- Flash to hardware and run a representative project (e.g. a Scratch project
  with sprites, sound, and keyboard input).
- Check the serial monitor for new warnings or asserts.

When fixing a runtime bug, attach the **project ID** or `.sb3` that
reproduces the bug, plus the serial log.

## Reporting bugs / requesting features

Use the issue templates under `.github/ISSUE_TEMPLATE/`. Please include:

- Hardware target (Tab5 / Stamp P4 / S3)
- ESP-IDF version
- Steps to reproduce, expected vs. actual behavior
- Serial log excerpts, if any

Security-related issues: see [SECURITY.md](./SECURITY.md).
