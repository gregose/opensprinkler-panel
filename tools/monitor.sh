#!/usr/bin/env bash

set -euo pipefail

baud="115200"
port=""

usage() {
  cat <<'EOF'
Usage: tools/monitor.sh [--port <device>] [--baud <rate>]

Open a serial monitor for the ESP32 panel.
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
    --port)
      port="${2:-}"
      shift 2
      ;;
    --baud)
      baud="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      printf 'Unknown argument: %s\n\n' "$1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$port" ]]; then
  port="$(detect_port)"
fi

exec python3 -m serial.tools.miniterm "$port" "$baud"
