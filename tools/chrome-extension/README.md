# sb3tab QR — Chrome extension

[**日本語版はこちら**](README.ja.md)

Generate the QR codes that sb3tab's on-device scanner reads:

- **WiFi QR** — encodes an SSID + password pair so sb3tab can join your
  network and save the credentials to `/sd/wifi.txt`.
- **Project QR** — when you're already on a `scratch.mit.edu/projects/<id>`
  page, the popup auto-detects the project ID and produces a QR that tells
  sb3tab to stream that project from the Scratch CDN to
  `/sd/games/<id>/`.

Both QR codes use the same JSON-in-a-QR format that
[`tools/gen_qr.py`](../gen_qr.py) generates, so the device-side flow is
identical no matter which path you used.

## Install (unpacked, developer mode)

The extension isn't published to the Chrome Web Store. Load it manually:

1. Open `chrome://extensions/`
2. Toggle **Developer mode** on (top-right).
3. Click **Load unpacked**.
4. Pick this `tools/chrome-extension/` directory.
5. Pin the **sb3tab QR** action so it's reachable from the toolbar.

## Use

### Load a Scratch project (the everyday flow)

1. Open the project page on
   `https://scratch.mit.edu/projects/<id>` in a browser tab.
2. Click the extension's toolbar icon. The popup opens on the
   **Project** tab and picks up the current page automatically.
3. Scan the generated QR with sb3tab. The runtime downloads
   `project.json` plus every asset to `/sd/games/<id>/` and starts the
   project as soon as the transfer finishes.

### Add a WiFi network to the device (one-time setup)

1. Click the extension's toolbar icon.
2. Switch to the **WiFi** tab.
3. Enter your network name and password.
4. Click **Generate QR** and scan it with sb3tab's on-device QR scanner.
5. The device joins the network and writes the credentials to
   `/sd/wifi.txt` so it auto-connects on subsequent boots.

## Privacy

The extension only reads the URL of the active tab to find the Scratch
project ID, and only stores the most recently entered SSID/password
locally in `chrome.storage` so you don't have to retype them. Nothing
is sent off-device — QR generation is done entirely in the popup.
