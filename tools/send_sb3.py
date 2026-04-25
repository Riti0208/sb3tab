#!/usr/bin/env python3
"""
Send a Scratch project to ESP32-P4 via serial.
Extracts .sb3 on host and sends project.json + assets individually.

Usage:
  python send_sb3.py <project_id_or_file> [serial_port]
"""

import sys
import os
import time
import json
import zipfile
import io
import urllib.request
import serial

DEFAULT_PORT = "/dev/tty.usbmodem1101"
BAUD_RATE = 115200


def download_sb3(project_id: str) -> bytes:
    """Download .sb3 file from Scratch API."""
    print(f"Fetching project token for {project_id}...")
    api_url = f"https://api.scratch.mit.edu/projects/{project_id}"
    with urllib.request.urlopen(api_url) as resp:
        data = json.loads(resp.read())

    token = data.get("project_token")
    if not token:
        raise RuntimeError("No project_token in API response")

    project_url = f"https://projects.scratch.mit.edu/{project_id}?token={token}"
    print("Downloading project JSON...")
    with urllib.request.urlopen(project_url) as resp:
        project_json = resp.read()

    proj = json.loads(project_json)
    assets = set()
    for target in proj.get("targets", []):
        for costume in target.get("costumes", []):
            md5ext = costume.get("md5ext") or costume.get("assetId", "") + "." + costume.get("dataFormat", "svg")
            if md5ext:
                assets.add(md5ext)
        for sound in target.get("sounds", []):
            md5ext = sound.get("md5ext") or sound.get("assetId", "") + "." + sound.get("dataFormat", "wav")
            if md5ext:
                assets.add(md5ext)

    sb3_buf = io.BytesIO()
    with zipfile.ZipFile(sb3_buf, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("project.json", project_json)
        for asset_name in sorted(assets):
            asset_url = f"https://assets.scratch.mit.edu/internalapi/asset/{asset_name}/get/"
            print(f"  Downloading {asset_name}...")
            try:
                with urllib.request.urlopen(asset_url) as resp:
                    asset_data = resp.read()
                zf.writestr(asset_name, asset_data)
                print(f"    {len(asset_data)} bytes")
            except Exception as e:
                print(f"    Failed: {e}")

    return sb3_buf.getvalue()


def send_data(ser, data: bytes):
    """Send raw data in 64-byte chunks."""
    sent = 0
    while sent < len(data):
        end = min(sent + 64, len(data))
        ser.write(data[sent:end])
        sent = end
        time.sleep(0.005)


def wait_for(ser, expected: str, timeout: float = 10.0) -> bool:
    """Wait for a specific response from P4."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line:
            print(f"  < {line}")
        if expected in line:
            return True
        if "ERR" in line:
            return False
    return False


def send_project(sb3_data: bytes, port: str):
    """Extract .sb3 and send project data to ESP32."""
    # Extract .sb3 on host
    print("Extracting .sb3...")
    with zipfile.ZipFile(io.BytesIO(sb3_data)) as zf:
        project_json = zf.read("project.json")
        assets = {}
        for name in zf.namelist():
            if name != "project.json":
                assets[name] = zf.read(name)

    print(f"  project.json: {len(project_json)} bytes")
    print(f"  Assets: {len(assets)}")

    # Connect to P4
    print(f"Opening {port}...")
    ser = serial.Serial(port, BAUD_RATE, timeout=2)
    time.sleep(1)
    ser.reset_input_buffer()

    # Drain and wait for WAITING
    for _ in range(10):
        line = ser.readline().decode(errors="ignore").strip()
        if line:
            print(f"  < {line}")
        if "WAITING" in line:
            break

    # Send project.json
    print(f"Sending project.json ({len(project_json)} bytes)...")
    ser.write(f"JSON:{len(project_json)}\n".encode())
    if not wait_for(ser, "READY"):
        print("Failed to get READY for JSON")
        ser.close()
        return False
    send_data(ser, project_json)
    if not wait_for(ser, "OK", 30):
        print("Failed to send JSON")
        ser.close()
        return False

    # Send assets
    for name, data in sorted(assets.items()):
        print(f"Sending {name} ({len(data)} bytes)...")
        ser.write(f"ASSET:{name}:{len(data)}\n".encode())
        if not wait_for(ser, "READY"):
            print(f"Failed to get READY for {name}")
            ser.close()
            return False
        send_data(ser, data)
        timeout = max(30, len(data) / 10000 + 10)  # Scale with size
        if not wait_for(ser, "OK", timeout):
            print(f"Failed to send {name}")
            ser.close()
            return False

    # Start execution
    print("Starting project...")
    ser.write(b"START\n")

    # Read output
    print("\n--- ESP32 output ---")
    try:
        while True:
            line = ser.readline().decode(errors="ignore").strip()
            if line:
                print(line)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    return True


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    source = sys.argv[1]
    port = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_PORT

    if os.path.isfile(source):
        print(f"Reading local file: {source}")
        with open(source, "rb") as f:
            sb3_data = f.read()
    else:
        sb3_data = download_sb3(source)

    send_project(sb3_data, port)


if __name__ == "__main__":
    main()
