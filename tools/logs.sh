#!/usr/bin/env bash

set -euo pipefail

# Use the repo-local flash toolchain venv if present (see tools/setup.sh).
# shellcheck source=tools/_venv.sh
source "$(dirname "${BASH_SOURCE[0]}")/_venv.sh"

# tools/logs.sh — stream the TCP log port to stdout + a log file.
#
# The firmware opens a single-client TCP server on port 2323 when the NVS
# dev_log flag is true.  This script connects to that port, tees every line
# to logs/serial.log, and reconnects after OTA reboots or network drops.
# SO_KEEPALIVE lets the OS detect hard resets; a silence window (~16 s) also
# triggers a reconnect so a stale half-open socket never blocks the firmware's
# single-client slot.

host="ospanel.local"
port="2323"
log="logs/serial.log"

usage() {
  cat <<'EOF'
Usage: tools/logs.sh [--host <ip-or-hostname>] [--port <port>] [--log <path>]

Stream the TCP log port to stdout and a log file. Reconnects after OTA reboots,
hard device resets, or any period of silence longer than ~16 s.
The log server is active only when dev_log is enabled in the config portal.

Options:
  --host <host>   Device hostname or IP (default: ospanel.local).
  --port <port>   TCP log port (default: 2323).
  --log <path>    Log file to append to (default: logs/serial.log).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) host="${2:-}"; shift 2 ;;
    --port) port="${2:-}"; shift 2 ;;
    --log)  log="${2:-}";  shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown argument: %s\n\n' "$1" >&2; usage >&2; exit 1 ;;
  esac
done

log_dir="$(dirname "$log")"
[[ "$log_dir" == "." || -z "$log_dir" ]] || mkdir -p "$log_dir"

printf 'Streaming %s:%s -> %s (reconnects on silence or disconnect)\n' "$host" "$port" "$log" >&2

HOST="$host" PORT="$port" LOG="$log" exec python3 - <<'PY'
import os
import socket
import sys
import time

host = os.environ["HOST"]
port = int(os.environ["PORT"])
log_path = os.environ["LOG"]

CONNECT_TIMEOUT = 5    # seconds to wait for initial TCP connection
RECV_BUFFER_SIZE = 256  # bytes per recv() call
RECV_TIMEOUT = 2.0      # seconds per recv() call
SILENCE_LIMIT = 8       # consecutive timeouts (~16 s) before reconnecting

log = open(log_path, "a", buffering=1)
print(f"--- logs start {time.strftime('%Y-%m-%dT%H:%M:%S')} {host}:{port} ---",
      file=log, flush=True)

try:
    while True:
        try:
            sock = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT)
        except (OSError, socket.timeout) as exc:
            print(f"[logs] cannot connect to {host}:{port}: {exc}; retrying...",
                  file=sys.stderr)
            time.sleep(2.0)
            continue

        note = f"--- connected {time.strftime('%Y-%m-%dT%H:%M:%S')} ---"
        print(note)
        print(note, file=log, flush=True)

        # Enable SO_KEEPALIVE so the OS detects half-open sockets after a
        # hard device reboot (ESP.restart() sends no FIN/RST).
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        try:
            if hasattr(socket, 'TCP_KEEPALIVE'):    # macOS
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPALIVE, 10)
            if hasattr(socket, 'TCP_KEEPIDLE'):     # Linux
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
            if hasattr(socket, 'TCP_KEEPINTVL'):    # Linux
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 5)
        except OSError:
            pass

        sock.settimeout(RECV_TIMEOUT)
        buf = b""
        silence_count = 0
        try:
            while True:
                try:
                    chunk = sock.recv(RECV_BUFFER_SIZE)
                except socket.timeout:
                    silence_count += 1
                    if silence_count >= SILENCE_LIMIT:
                        break  # ~16 s of silence; treat as disconnect
                    continue
                if not chunk:
                    break
                silence_count = 0
                buf += chunk
                # Flush complete lines to stdout + log.
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    sys.stdout.write(text + "\n")
                    sys.stdout.flush()
                    log.write(text + "\n")
                    log.flush()
        except (OSError, ConnectionResetError):
            pass
        finally:
            try:
                sock.close()
            except OSError:
                pass

        disc = f"--- disconnected {time.strftime('%Y-%m-%dT%H:%M:%S')}, reconnecting ---"
        print(disc, file=sys.stderr)
        print(disc, file=log, flush=True)
        time.sleep(1.0)
except KeyboardInterrupt:
    pass
finally:
    print(f"--- logs stop {time.strftime('%Y-%m-%dT%H:%M:%S')} ---",
          file=log, flush=True)
PY
