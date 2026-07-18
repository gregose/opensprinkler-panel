#!/usr/bin/env bash

set -euo pipefail

# Use the repo-local flash toolchain venv if present (see tools/setup.sh).
# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

# Background-friendly serial monitor for the local flash/debug bridge.
#
# Unlike a bare miniterm, this monitor:
#   * streams every line to a log file (append + flush) AND to stdout, so an
#     agent can grep the log while a human watches the terminal;
#   * pulses DTR/RTS on connect to capture a full boot banner every time;
#   * AUTO-YIELDS the serial port whenever tools/flash.sh starts a flash, then
#     reconnects (and re-captures the boot) once the flash finishes.
#
# Coordination with flash.sh happens through a small state directory:
#   <state-dir>/pause   flasher requests the monitor release the port
#   <state-dir>/active  monitor is currently holding the port open

baud="115200"
port=""
log="logs/serial.log"
state_dir="${MON_STATE_DIR:-.serial-monitor}"
reset_on_connect="1"

usage() {
  cat <<'EOF'
Usage: tools/monitor.sh [--port <device>] [--baud <rate>] [--log <path>]
                        [--state-dir <path>] [--no-reset]

Stream the ESP32 serial output to stdout and a log file. Automatically releases
the port while tools/flash.sh is flashing, then reconnects.

Options:
  --port <device>     Serial device (auto-detected if omitted).
  --baud <rate>       Baud rate (default 115200).
  --log <path>        Log file to append to (default logs/serial.log).
  --state-dir <path>  Coordination dir shared with flash.sh (default .serial-monitor).
  --no-reset          Do not pulse DTR/RTS on connect (attach without rebooting).
EOF
}

detect_port() {
  python3 - <<'PY'
import re
import sys
from serial.tools import list_ports

ports = list(list_ports.comports())
if not ports:
    sys.exit("No serial ports found. Pass --port explicitly.")

preferred = []
fallback = []
pattern = re.compile(r"(usb|uart|serial|acm|cp210|ch340|ftdi|silicon labs|wch)", re.I)
for port in ports:
    haystack = " ".join(
        value for value in [port.device, port.description, port.manufacturer, port.hwid] if value
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
    --port) port="${2:-}"; shift 2 ;;
    --baud) baud="${2:-}"; shift 2 ;;
    --log) log="${2:-}"; shift 2 ;;
    --state-dir) state_dir="${2:-}"; shift 2 ;;
    --no-reset) reset_on_connect="0"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n\n' "$1" >&2; usage >&2; exit 1 ;;
  esac
done

if [[ -z "$port" ]]; then
  port="$(detect_port)"
fi

mkdir -p "$state_dir"
log_dir="$(dirname "$log")"
[[ "$log_dir" == "." || -z "$log_dir" ]] || mkdir -p "$log_dir"

printf 'Monitoring %s @ %s -> %s (auto-yields on flash)\n' "$port" "$baud" "$log" >&2

PORT="$port" BAUD="$baud" LOG="$log" STATE_DIR="$state_dir" RESET="$reset_on_connect" \
  exec python3 - <<'PY'
import os
import sys
import time

import serial

port = os.environ["PORT"]
baud = int(os.environ["BAUD"])
log_path = os.environ["LOG"]
state_dir = os.environ["STATE_DIR"]
reset_on_connect = os.environ.get("RESET", "1") == "1"

pause_file = os.path.join(state_dir, "pause")
active_file = os.path.join(state_dir, "active")


def paused():
    return os.path.exists(pause_file)


def set_active(is_active):
    try:
        if is_active:
            open(active_file, "w").close()
        elif os.path.exists(active_file):
            os.remove(active_file)
    except OSError:
        pass


def pulse_reset(ser):
    # Classic esptool auto-reset: assert then release via DTR/RTS.
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.05)
    except (OSError, serial.SerialException):
        pass


def open_port():
    ser = serial.Serial(port, baud, timeout=0.2)
    if reset_on_connect:
        pulse_reset(ser)
    return ser


log = open(log_path, "a", buffering=1)
print(f"--- monitor start {time.strftime('%Y-%m-%dT%H:%M:%S')} {port}@{baud} ---",
      file=log, flush=True)

try:
    while True:
        if paused():
            set_active(False)
            time.sleep(0.3)
            continue
        try:
            ser = open_port()
        except (OSError, serial.SerialException) as exc:
            set_active(False)
            print(f"[monitor] cannot open {port}: {exc}; retrying...", file=sys.stderr)
            time.sleep(1.0)
            continue

        set_active(True)
        note = f"--- connected {time.strftime('%Y-%m-%dT%H:%M:%S')} ---"
        print(note)
        print(note, file=log, flush=True)
        try:
            while True:
                if paused():
                    break
                try:
                    chunk = ser.readline()
                except (OSError, serial.SerialException):
                    break
                if not chunk:
                    continue
                text = chunk.decode("utf-8", "replace").rstrip("\r\n")
                sys.stdout.write(text + "\n")
                sys.stdout.flush()
                log.write(text + "\n")
                log.flush()
        finally:
            set_active(False)
            try:
                ser.close()
            except (OSError, serial.SerialException):
                pass
except KeyboardInterrupt:
    pass
finally:
    set_active(False)
    print(f"--- monitor stop {time.strftime('%Y-%m-%dT%H:%M:%S')} ---",
          file=log, flush=True)
PY
