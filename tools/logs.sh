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
# SO_KEEPALIVE lets the OS detect a hard reset: after a reboot the device RSTs
# the first keepalive probe to the stale half-open socket, which surfaces as a
# recv() error and triggers a reconnect (so the firmware's single-client slot
# never stays wedged). There is deliberately NO app-level silence timer — the
# firmware emits no heartbeat, so an idle-but-healthy board looks identical to a
# dead one and any finite silence window would churn needless reconnects.

host="ospanel.local"
port="2323"
log="logs/serial.log"

usage() {
  cat <<'EOF'
Usage: tools/logs.sh [--host <ip-or-hostname>] [--port <port>] [--log <path>]

Stream the TCP log port to stdout and a log file. Reconnects after OTA reboots,
hard device resets, and network drops (via SO_KEEPALIVE); stays quietly connected
while an idle board sends nothing. The log server is active only when dev_log is
enabled in the config portal.

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

printf 'Streaming %s:%s -> %s (reconnects on reboot/disconnect)\n' "$host" "$port" "$log" >&2

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
RECV_TIMEOUT = 2.0      # seconds per recv() call (keeps Ctrl-C responsive)

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

        # Enable SO_KEEPALIVE so the OS detects a half-open socket after a hard
        # device reboot (ESP.restart() sends no FIN/RST): the rebooted device
        # RSTs the first keepalive probe, surfacing as a recv() error below.
        # This is the sole reconnect trigger — see the note at the top of the
        # file for why there is no app-level silence timer.
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        try:
            if hasattr(socket, 'TCP_KEEPALIVE'):    # macOS: idle before probing
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPALIVE, 10)
            if hasattr(socket, 'TCP_KEEPIDLE'):     # Linux: idle before probing
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPIDLE, 10)
            if hasattr(socket, 'TCP_KEEPINTVL'):    # macOS + Linux: probe spacing
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPINTVL, 5)
            if hasattr(socket, 'TCP_KEEPCNT'):      # macOS + Linux: probes before drop
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_KEEPCNT, 4)
        except OSError:
            pass

        sock.settimeout(RECV_TIMEOUT)
        buf = b""
        try:
            while True:
                try:
                    chunk = sock.recv(RECV_BUFFER_SIZE)
                except socket.timeout:
                    # Idle: healthy board with nothing to say. Keep waiting;
                    # SO_KEEPALIVE (not silence) is what detects a dead peer.
                    continue
                if not chunk:
                    break
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
