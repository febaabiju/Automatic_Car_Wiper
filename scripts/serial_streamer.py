"""
ESP32 Serial-to-Cloud Real-Time Telemetry Forwarder
Reads live sensor readings from ESP32 over USB Serial (COM3) and
streams them directly to the live Render FastAPI backend.

Usage:
    python scripts/serial_streamer.py [PORT]
    (Default port: COM3, baud: 115200)
"""

import sys
import json
import time
import re
import urllib.request
import urllib.error

try:
    import serial
except ImportError:
    print("Error: pyserial is required. Install via: pip install pyserial")
    sys.exit(1)

DEFAULT_PORT = "COM3"
DEFAULT_BAUD = 115200
BACKEND_URL = "https://automatic-car-wiper.onrender.com/api/readings"

def post_reading(payload: dict):
    data_bytes = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        BACKEND_URL,
        data=data_bytes,
        headers={"Content-Type": "application/json"},
        method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=3.0) as resp:
            if resp.status in (200, 201):
                body = json.loads(resp.read().decode())
                reading_id = body.get("data", {}).get("id", "?")
                print(f" [CLOUD] Streamed to Render: ID #{reading_id} | Rain: {payload.get('rain_value')} ADC ({payload.get('rain_status')}) | Wiper: {payload.get('wiper_speed')}")
    except Exception as exc:
        print(f" [CLOUD] Upload error: {exc}")

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    print("=" * 60)
    print(f"   Automatic Car Wiper - Live Serial Streamer")
    print(f"   Forwarding {port} -> {BACKEND_URL}")
    print("=" * 60)

    try:
        ser = serial.Serial(port, DEFAULT_BAUD, timeout=1.0)
        ser.dtr = False
        ser.rts = False
    except Exception as err:
        print(f"Error opening serial port {port}: {err}")
        return

    print(f"Connected to {port} at {DEFAULT_BAUD} baud. Listening for telemetry...\n")

    current_rain_val = 4095
    current_status = "Dry"
    current_speed = "Off"
    wiper_on = False

    try:
        while True:
            raw_line = ser.readline()
            if not raw_line:
                continue
            line = raw_line.decode("utf-8", errors="replace").strip()
            if not line:
                continue

            # Echo device serial
            print(f"[ESP32] {line}")

            # Check for direct JSON telemetry marker
            if "TELEMETRY:" in line:
                try:
                    json_str = line.split("TELEMETRY:", 1)[1].strip()
                    payload = json.loads(json_str)
                    post_reading(payload)
                except Exception as e:
                    print(f"JSON Parse warning: {e}")
                continue

            # Parse standard serial messages if TELEMETRY tag is not present
            if "Rain Value:" in line:
                match = re.search(r"Rain Value:\s*(\d+)", line)
                if match:
                    current_rain_val = int(match.group(1))

            if "HEAVY RAIN" in line:
                current_status = "Heavy Rain"
                current_speed = "Fast"
                wiper_on = True
            elif "LIGHT RAIN" in line:
                current_status = "Light Rain"
                current_speed = "Slow"
                wiper_on = True
            elif "DRY - WIPER OFF" in line:
                current_status = "Dry"
                current_speed = "Off"
                wiper_on = False

    except KeyboardInterrupt:
        print("\nStopping serial streamer.")
    finally:
        if ser.is_open:
            ser.close()

if __name__ == "__main__":
    main()

