#!/usr/bin/env python3
"""Generate QR codes for Scratcher Tab5.

Usage:
  python3 gen_qr.py wifi "SSID" "PASSWORD"
  python3 gen_qr.py project 1296865674
"""
import sys
import qrcode

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    mode = sys.argv[1]

    if mode == "wifi":
        ssid = sys.argv[2]
        password = sys.argv[3] if len(sys.argv) > 3 else ""
        data = f"W:{ssid}\n{password}"
        filename = "wifi_qr.png"
    elif mode == "project":
        project_id = sys.argv[2]
        data = f"S:{project_id}"
        filename = "project_qr.png"
    else:
        print(f"Unknown mode: {mode}")
        sys.exit(1)

    print(f"Data: {repr(data)}")
    print(f"UTF-8 bytes: {len(data.encode('utf-8'))}")

    qr = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L)
    qr.add_data(data)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    img.save(filename)
    print(f"Saved: {filename}")

if __name__ == "__main__":
    main()
