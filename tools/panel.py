#!/usr/bin/env python3
"""Bench probe client for the OpenSprinkler panel.

Talks to the firmware's dev-log TCP socket (port 2323, enabled by the `dev_log`
NVS flag) to pull a pixel-exact screenshot of whatever LVGL last drew and to
drive the UI with synthetic touch — no camera, no finger required.

Examples:
    tools/panel.py --host <panel-ip> shot -o screen.png
    tools/panel.py --host <panel-ip> tap 145 250
    tools/panel.py --host <panel-ip> down 40 40
    tools/panel.py --host <panel-ip> up

The wire protocol is defined in lib/bench_probe. Screenshot framing:
    \\x02SHOT <w> <h> 565\\n
    \\x02STRIP <x> <y> <w> <h>\\n<raw RGB565 little-endian bytes>   (repeated)
    \\x02END\\n
Any plain-text log bytes emitted before the SHOT header are discarded.
"""

import argparse
import socket
import struct
import sys
import time
import zlib

STX = 0x02
DEFAULT_PORT = 2323

# Brightness below which a whole frame is treated as "blank" (the panel blanks
# its backlight/screen after ~5 min idle, so a capture then comes back all-dark).
# The settled dark UI background is ~0x07/0x10/0x0f (max channel ~16), while any
# live screen always has bright content (white text 255, teal buttons), so a
# frame whose brightest channel is under this threshold means "asleep / no UI".
#
# Measured on real hardware (ESP32-3248S035R): a genuinely blanked frame is not
# pure black — a faint ~1px edge/bezel glow peaks at max channel 49, so the old
# threshold of 48 mis-classified a blanked panel as "awake" and auto-wake never
# fired. 64 clears that 49 floor with margin while staying far below live UI.
BLANK_MAX_CHANNEL = 64


class ProtocolError(Exception):
    pass


# --- pure helpers (unit-tested in tools/test_panel.py) ----------------------

def rgb565_to_rgb888(data: bytes, npx: int) -> bytes:
    """Convert `npx` little-endian RGB565 pixels to packed RGB888 bytes."""
    out = bytearray(npx * 3)
    for i in range(npx):
        v = data[i * 2] | (data[i * 2 + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[i * 3] = (r * 255 + 15) // 31
        out[i * 3 + 1] = (g * 255 + 31) // 63
        out[i * 3 + 2] = (b * 255 + 15) // 31
    return bytes(out)


def _read_exact(reader, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = reader.read(n - len(buf))
        if not chunk:
            raise ProtocolError("connection closed mid-frame")
        buf += chunk
    return bytes(buf)


def _read_control_line(reader) -> str:
    """Read bytes up to (not including) a newline; the STX is already consumed."""
    line = bytearray()
    while True:
        ch = reader.read(1)
        if not ch:
            raise ProtocolError("connection closed mid control line")
        if ch == b"\n":
            break
        line += ch
    return line.decode("ascii", "replace").strip()


def read_screenshot(reader):
    """Parse a screenshot from `reader` (any object with .read(n)).

    Returns (width, height, rgb888_bytes). Leading plain-text log output before
    the SHOT header is skipped.
    """
    # Scan for the SHOT header, discarding any preceding log text.
    width = height = 0
    while True:
        b = reader.read(1)
        if not b:
            raise ProtocolError("connection closed before SHOT header")
        if b[0] != STX:
            continue
        parts = _read_control_line(reader).split()
        if parts and parts[0] == "SHOT":
            width, height = int(parts[1]), int(parts[2])
            break
        # Some other control line before SHOT — ignore and keep scanning.

    frame = bytearray(width * height * 2)
    # Primary exit is the \x02END\n terminator. As defense-in-depth (so a
    # firmware regression that drops END can't hang the client forever), also
    # exit once every row has been covered by a strip.
    rows_seen = bytearray(height)
    rows_left = height
    while True:
        b = reader.read(1)
        # Skip any stray bytes until the next control marker (defensive).
        while b and b[0] != STX:
            b = reader.read(1)
        if not b:
            raise ProtocolError("connection closed mid frame")
        parts = _read_control_line(reader).split()
        if not parts:
            continue
        if parts[0] == "END":
            break
        if parts[0] == "STRIP":
            x, y, sw, sh = (int(v) for v in parts[1:5])
            data = _read_exact(reader, sw * sh * 2)
            for row in range(sh):
                dst = ((y + row) * width + x) * 2
                src = row * sw * 2
                frame[dst:dst + sw * 2] = data[src:src + sw * 2]
                yy = y + row
                if 0 <= yy < height and not rows_seen[yy]:
                    rows_seen[yy] = 1
                    rows_left -= 1
            if rows_left <= 0:
                break
        else:
            raise ProtocolError("unexpected control frame: %r" % parts[0])

    rgb = rgb565_to_rgb888(bytes(frame), width * height)
    return width, height, rgb


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    """Encode packed RGB888 pixels as a PNG (stdlib only, no Pillow)."""
    def chunk(typ: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def frame_is_blank(rgb: bytes) -> bool:
    """True if the frame has no bright content (panel asleep / screen blanked)."""
    return bool(rgb) and max(rgb) < BLANK_MAX_CHANNEL


# --- socket I/O -------------------------------------------------------------

def _connect(host: str, port: int, timeout: float):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def _capture_once(host, port, timeout):
    sock = _connect(host, port, timeout)
    try:
        reader = sock.makefile("rb")
        sock.sendall(b"SHOT\n")
        try:
            return read_screenshot(reader)
        except socket.timeout:
            raise ProtocolError(
                "timed out after %gs waiting for screen data — is dev_log "
                "enabled and is another client holding the single :2323 slot?"
                % timeout)
    finally:
        sock.close()


def do_shot(host, port, out_path, timeout, wake=True):
    width, height, rgb = _capture_once(host, port, timeout)

    # The panel blanks after ~5 min idle; a capture then comes back all-dark.
    # Only when we actually see a blank frame do we send a wake tap and retry —
    # the firmware consumes the first touch of a sleeping panel as the wake
    # (it is NOT dispatched to the UI), so this cannot actuate a control. We
    # never tap a non-blank (awake) frame, so a live screen is untouched.
    if wake and frame_is_blank(rgb):
        print("[panel] captured frame is blank — panel looks asleep; "
              "sending a wake tap and retrying...", file=sys.stderr)
        try:
            _send_line(host, port, "TAP 2 2", timeout)
            time.sleep(0.6)
            width, height, rgb = _capture_once(host, port, timeout)
        except OSError:
            pass
        if frame_is_blank(rgb):
            print("[panel] warning: frame still blank after wake — board may be "
                  "off, mid-boot, or genuinely showing a black screen.",
                  file=sys.stderr)

    png = encode_png(width, height, rgb)
    if out_path == "-":
        sys.stdout.buffer.write(png)
    else:
        with open(out_path, "wb") as f:
            f.write(png)
        print("wrote %s (%dx%d)" % (out_path, width, height), file=sys.stderr)


def _send_line(host, port, line, timeout):
    sock = _connect(host, port, timeout)
    try:
        sock.sendall((line + "\n").encode("ascii"))
    finally:
        sock.close()


def do_send(host, port, line, timeout):
    _send_line(host, port, line, timeout)
    print("sent: %s" % line, file=sys.stderr)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", required=True, help="panel IP or mDNS name")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--timeout", type=float, default=10.0)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("shot", help="capture the screen to a PNG")
    s.add_argument("-o", "--out", default="screen.png", help="output PNG ('-' = stdout)")
    s.add_argument("--no-wake", dest="wake", action="store_false",
                   help="don't send a wake tap if the captured frame is blank")

    for name, help_ in (("tap", "click at x y"), ("down", "press-hold at x y"),
                        ("move", "move while pressed to x y")):
        sp = sub.add_parser(name, help=help_)
        sp.add_argument("x", type=int)
        sp.add_argument("y", type=int)
    sub.add_parser("up", help="release touch")
    sr = sub.add_parser("raw", help="send a raw command line")
    sr.add_argument("line")

    args = p.parse_args(argv)
    if args.cmd == "shot":
        do_shot(args.host, args.port, args.out, args.timeout, wake=args.wake)
    elif args.cmd in ("tap", "down", "move"):
        do_send(args.host, args.port, "%s %d %d" % (args.cmd.upper(), args.x, args.y),
                args.timeout)
    elif args.cmd == "up":
        do_send(args.host, args.port, "UP", args.timeout)
    elif args.cmd == "raw":
        do_send(args.host, args.port, args.line, args.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
