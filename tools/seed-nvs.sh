#!/usr/bin/env bash
# seed-nvs.sh — Write NVS credentials into the cyd-35r-diag firmware via USB
# serial, using the diagnostic firmware's interactive 's' command.
#
# After a full USB flash (tools/flash.sh) wipes NVS, run this script to seed
# credentials so the production cyd-35r firmware can connect to Wi-Fi and load
# the OpenSprinkler station list before M4 provisioning exists.
#
# The OS password is sent as plaintext over the local USB link; the device
# computes the MD5 hash on-chip via MD5Builder and stores only the hash —
# plaintext is never persisted in NVS.
#
# Requires: pyserial  (pip install "pyserial==3.5")
# Usage: ./tools/seed-nvs.sh [--port <device>]
#                             --ssid <wifi_ssid>
#                             --pass <wifi_password>
#                             --host <os_host>
#                             --ospw <os_plaintext_password>
#
# All credential flags are optional — omit any key you do not want to change.
# Use --port to override the auto-detected serial device (same logic as
# tools/flash.sh).

set -euo pipefail

# Use the repo-local flash toolchain venv if present (see tools/setup.sh).
# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

port=""
ssid=""
wifi_pass=""
os_host=""
os_pw=""

usage() {
  cat <<'EOF'
Usage: tools/seed-nvs.sh [--port <device>]
                          [--ssid <wifi_ssid>]
                          [--pass <wifi_password>]
                          [--host <os_host>]
                          [--ospw <os_plaintext_password>]

Seeds NVS credentials into the cyd-35r-diag firmware running on the connected
device. Omit any flag to leave that NVS key unchanged.

The OS password (--ospw) is sent as plaintext over the local USB serial link.
The device hashes it to MD5 on-chip before storing — plaintext is never written
to NVS.

Requires pyserial (already installed as a flash.sh dependency):
  pip install "pyserial==3.5"
EOF
}

detect_port() {
  python3 - <<'PY'
import re, sys
from serial.tools import list_ports

ports = list(list_ports.comports())
if not ports:
    sys.exit("No serial ports found. Pass --port explicitly.")

preferred = []
fallback = []
pattern = re.compile(r"(usb|uart|serial|acm|cp210|ch340|ftdi|silicon labs|wch)", re.I)
for port in ports:
    haystack = " ".join(
        v for v in [port.device, port.description, port.manufacturer, port.hwid] if v
    )
    if pattern.search(haystack):
        preferred.append(port.device)
    fallback.append(port.device)

devices = sorted(preferred or fallback)
print(devices[0])
PY
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) port="${2:-}";      shift 2 ;;
    --ssid) ssid="${2:-}";      shift 2 ;;
    --pass) wifi_pass="${2:-}"; shift 2 ;;
    --host) os_host="${2:-}";   shift 2 ;;
    --ospw) os_pw="${2:-}";     shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n\n' "$1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "$ssid" && -z "$wifi_pass" && -z "$os_host" && -z "$os_pw" ]]; then
  printf 'No credentials supplied — nothing to do.\n\n' >&2
  usage >&2
  exit 1
fi

if [[ -z "$port" ]]; then
  port="$(detect_port)"
fi

printf 'Seeding NVS on %s\n' "$port"

python3 - "$port" "$ssid" "$wifi_pass" "$os_host" "$os_pw" <<'PY'
import sys, time, serial

port    = sys.argv[1]
ssid    = sys.argv[2]
wifi_pw = sys.argv[3]
os_host = sys.argv[4]
os_pw   = sys.argv[5]

def set_nvs_key(ser, key, value):
    """Send 's' then 'key value\\n' to set one NVS key on the device."""
    if not value:
        print(f"  skip {key} (no value)")
        return
    # Flush stale input before issuing command.
    time.sleep(0.1)
    ser.reset_input_buffer()

    # Trigger the interactive NVS setter in the diag firmware.
    ser.write(b's')

    # Wait for the "NVS>" prompt before sending data.
    deadline = time.time() + 3.0
    buf = b''
    while time.time() < deadline:
        chunk = ser.read(max(ser.in_waiting, 1))
        buf += chunk
        if b'NVS>' in buf:
            break
        time.sleep(0.05)

    # Send the key/value line.
    ser.write(f"{key} {value}\n".encode())

    # Collect the device's response (echoed set confirmation).
    time.sleep(0.4)
    resp = ser.read(ser.in_waiting or 1)
    print(resp.decode('utf-8', errors='replace'), end='', flush=True)

keys_to_set = [
    ("wifi_ssid", ssid),
    ("wifi_pass", wifi_pw),
    ("os_host",   os_host),
    ("os_pw_md5", os_pw),
]

with serial.Serial(port, 115200, timeout=2) as ser:
    time.sleep(0.5)   # let the device settle after DTR/RTS toggle
    ser.reset_input_buffer()

    # Print current NVS state before changes.
    ser.write(b'n')
    time.sleep(0.5)
    before = ser.read(ser.in_waiting or 1)
    print("--- NVS before ---")
    print(before.decode('utf-8', errors='replace'))

    for k, v in keys_to_set:
        set_nvs_key(ser, k, v)

    # Print final NVS state after changes.
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(b'n')
    time.sleep(0.5)
    after = ser.read(ser.in_waiting or 1)
    print("\n--- NVS after ---")
    print(after.decode('utf-8', errors='replace'))

print("Done.")
PY
