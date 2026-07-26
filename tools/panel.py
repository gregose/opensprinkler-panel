#!/usr/bin/env python3
"""Bench probe client for the OpenSprinkler panel.

Talks to the firmware's dev-log TCP socket (port 2323, enabled by the `dev_log`
NVS flag) to pull a pixel-exact screenshot of whatever LVGL last drew and to
drive the UI with synthetic touch — no camera, no finger required.

Examples:
    tools/panel.py --host 192.168.1.246 shot -o screen.png
    tools/panel.py --host ospanel.local tap 145 250
    tools/panel.py --host 192.168.1.246 down 40 40
    tools/panel.py --host 192.168.1.246 up

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
import zlib

STX = 0x02
DEFAULT_PORT = 2323


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


# --- socket I/O -------------------------------------------------------------

def _connect(host: str, port: int, timeout: float):
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    return sock


def do_shot(host, port, out_path, timeout):
    sock = _connect(host, port, timeout)
    try:
        reader = sock.makefile("rb")
        sock.sendall(b"SHOT\n")
        try:
            width, height, rgb = read_screenshot(reader)
        except socket.timeout:
            raise ProtocolError(
                "timed out after %gs waiting for screen data — is dev_log "
                "enabled and is another client holding the single :2323 slot?"
                % timeout)
    finally:
        sock.close()
    png = encode_png(width, height, rgb)
    if out_path == "-":
        sys.stdout.buffer.write(png)
    else:
        with open(out_path, "wb") as f:
            f.write(png)
        print("wrote %s (%dx%d)" % (out_path, width, height), file=sys.stderr)


def do_send(host, port, line, timeout):
    sock = _connect(host, port, timeout)
    try:
        sock.sendall((line + "\n").encode("ascii"))
    finally:
        sock.close()
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
        do_shot(args.host, args.port, args.out, args.timeout)
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
