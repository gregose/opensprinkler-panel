#!/usr/bin/env bash
# tools/nvs.sh — back up and restore the device's NVS partition over USB.
#
# WHY THIS EXISTS
#   tools/flash.sh writes merged-firmware.bin at 0x0, which WIPES NVS (Wi-Fi
#   creds, os_host, os_pw_md5, ota_pass, touch calibration, dev_log). The
#   preferred dev loop is OTA (tools/ota.sh), which rewrites only the app
#   partition and preserves NVS — but when you must USB-flash (bootstrap, empty
#   NVS, partition-table change, brick recovery), this script lets you snapshot
#   the whole NVS partition once and restore it instantly afterwards, so you get
#   straight back to a working dev-loop state without re-running the config
#   portal or seed-nvs.sh.
#
#   Backup/restore operates on the raw NVS partition at 0x9000, length 0x5000
#   (20480 B), matching the min_spiffs.csv partition table.
#
# SECURITY — READ THIS
#   An NVS backup contains SENSITIVE secrets in the clear: your Wi-Fi PSK, the
#   OpenSprinkler password hash, and the OTA password. Backups are written to
#   .nvs-backups/ (git-ignored) and NEVER belong in the repository, a gist, an
#   artifact, or anywhere shared. Treat the .bin like a password file.
#
# USAGE
#   ./tools/nvs.sh backup  [--port <device>] [--out <file>]
#   ./tools/nvs.sh restore <file> [--port <device>]
#
# The CH340 bridge on this board is only reliable at 115200 for read-flash
# (460800 throws "Invalid head of packet"), so this script pins 115200.

set -euo pipefail

# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

NVS_OFFSET="0x9000"
NVS_SIZE="0x5000"   # 20480 bytes
BAUD="115200"
backup_dir="$repo_root/.nvs-backups"

port=""
out=""

usage() {
  cat <<'EOF'
Usage:
  tools/nvs.sh backup  [--port <device>] [--out <file>]
  tools/nvs.sh restore <file> [--port <device>]

backup   Read the NVS partition (0x9000, 0x5000 B) off the board into a .bin.
         Default output: .nvs-backups/nvs-YYYYmmdd-HHMMSS.bin (git-ignored).
restore  Write a previously captured NVS .bin back to 0x9000.

--port   Override the auto-detected serial device (same logic as flash.sh).

SECURITY: NVS backups contain your Wi-Fi PSK, OS password hash, and OTA
password in the clear. Never commit, upload, or share them.
EOF
}

detect_port() {
  python3 - <<'PY'
import re, sys
from serial.tools import list_ports

ports = list(list_ports.comports())
if not ports:
    sys.exit("No serial ports found. Pass --port explicitly.")

preferred, fallback = [], []
pattern = re.compile(r"(usb|uart|serial|acm|cp210|ch340|ftdi|silicon labs|wch)", re.I)
for p in ports:
    haystack = " ".join(v for v in [p.device, p.description, p.manufacturer, p.hwid] if v)
    (preferred if pattern.search(haystack) else fallback).append(p.device)

print(sorted(preferred or fallback)[0])
PY
}

[[ $# -ge 1 ]] || { usage >&2; exit 1; }
cmd="$1"; shift

case "$cmd" in
  backup)
    while [[ $# -gt 0 ]]; do
      case "$1" in
        --port) port="${2:-}"; shift 2 ;;
        --out)  out="${2:-}";  shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n\n' "$1" >&2; usage >&2; exit 1 ;;
      esac
    done
    [[ -n "$port" ]] || port="$(detect_port)"
    if [[ -z "$out" ]]; then
      mkdir -p "$backup_dir"
      out="$backup_dir/nvs-$(date +%Y%m%d-%H%M%S).bin"
    else
      mkdir -p "$(dirname "$out")"
    fi
    printf 'Reading NVS (%s, %s) from %s -> %s\n' "$NVS_OFFSET" "$NVS_SIZE" "$port" "$out"
    python3 -m esptool --chip esp32 --port "$port" --baud "$BAUD" \
      read-flash "$NVS_OFFSET" "$NVS_SIZE" "$out"
    chmod 600 "$out" 2>/dev/null || true
    printf '\nSaved %s (%s bytes).\n' "$out" "$(wc -c <"$out" | tr -d ' ')"
    printf 'SECURITY: this file holds Wi-Fi/OS/OTA secrets in the clear. Never commit or share it.\n'
    ;;
  restore)
    [[ $# -ge 1 ]] || { printf 'restore needs a <file>.\n\n' >&2; usage >&2; exit 1; }
    infile="$1"; shift
    while [[ $# -gt 0 ]]; do
      case "$1" in
        --port) port="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n\n' "$1" >&2; usage >&2; exit 1 ;;
      esac
    done
    [[ -f "$infile" ]] || { printf 'No such file: %s\n' "$infile" >&2; exit 1; }
    infile_size="$(wc -c <"$infile" | tr -d ' ')"
    expected_size="$((NVS_SIZE))"
    if [[ "$infile_size" != "$expected_size" ]]; then
      printf 'Refusing to restore: %s is %s bytes but the NVS partition is %s bytes (%s).\n' \
        "$infile" "$infile_size" "$expected_size" "$NVS_SIZE" >&2
      printf 'Only restore a .bin captured by "tools/nvs.sh backup" (same offset/size).\n' >&2
      exit 1
    fi
    [[ -n "$port" ]] || port="$(detect_port)"
    printf 'Restoring NVS from %s -> %s (%s)\n' "$infile" "$port" "$NVS_OFFSET"
    python3 -m esptool --chip esp32 --port "$port" --baud "$BAUD" \
      --before default-reset --after hard-reset \
      write-flash "$NVS_OFFSET" "$infile"
    printf '\nDone. NVS restored; the board reset into the restored config.\n'
    ;;
  -h|--help)
    usage; exit 0 ;;
  *)
    printf 'Unknown command: %s\n\n' "$cmd" >&2; usage >&2; exit 1 ;;
esac
