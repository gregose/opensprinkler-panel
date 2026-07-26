#!/usr/bin/env python3
"""Wire-level probe for the panel's dev-log screen-capture protocol (:2323).

Sends a single ``SHOT`` and dumps the raw framing so you can verify the
capture protocol on the wire without decoding an image: how many STRIP bands
arrived, any non-STRIP control lines, the trailing bytes, and — most usefully —
whether the ``\\x02END`` terminator actually made it onto the wire.

This is a diagnostic complement to ``panel.py shot`` (which decodes the PNG).
Use it when a capture hangs or returns via the coverage fallback, to tell a
"bytes never sent" bug apart from a "bytes sent, decode wrong" bug.

Stdlib only; no venv required.

Examples:
  python3 tools/probe.py --host <panel-ip>
  python3 tools/probe.py --host <panel-ip> --deadline 25
"""

import argparse
import socket
import sys
import time

STX = 0x02
DEFAULT_PORT = 2323


def probe(host, port, connect_timeout, idle_timeout, deadline):
    sock = socket.create_connection((host, port), timeout=connect_timeout)
    sock.settimeout(idle_timeout)
    sock.sendall(b"SHOT\n")

    t0 = time.time()
    buf = bytearray()
    while time.time() - t0 < deadline:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            print("[%.2fs] recv idle timeout (no data for %gs)"
                  % (time.time() - t0, idle_timeout), flush=True)
            break
        if not chunk:
            print("[%.2fs] EOF from panel" % (time.time() - t0), flush=True)
            break
        buf += chunk
    sock.close()

    n = len(buf)
    print("total bytes: %d" % n, flush=True)

    # Walk the framing: STX-led control lines; STRIP lines are followed by a
    # width*height*2 (RGB565) pixel payload we skip over.
    strips = 0
    controls = []
    j = 0
    while j < n:
        if buf[j] == STX:
            k = buf.find(b"\n", j + 1)
            if k == -1:
                break
            line = bytes(buf[j + 1:k]).decode("ascii", "replace")
            tok = line.split()
            if tok and tok[0] == "STRIP" and len(tok) >= 5:
                try:
                    sw, sh = int(tok[3]), int(tok[4])
                except ValueError:
                    controls.append((j, line))
                    j = k + 1
                    continue
                strips += 1
                j = k + 1 + sw * sh * 2
                continue
            controls.append((j, line))
            j = k + 1
            continue
        j += 1

    print("STRIP bands seen: %d" % strips, flush=True)
    print("non-STRIP control lines: %r" % (controls,), flush=True)

    tail = buf[-80:]
    print("TAIL hex:   %s" % tail.hex(), flush=True)
    print("TAIL ascii: %s"
          % "".join(chr(c) if 32 <= c < 127 else "." for c in tail), flush=True)

    has_end = b"\x02END" in buf
    print("contains b'\\x02END' (02 45 4e 44): %s" % has_end, flush=True)
    return has_end


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", required=True, help="panel IP or mDNS name")
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--connect-timeout", type=float, default=6.0,
                   help="TCP connect timeout (s)")
    p.add_argument("--idle-timeout", type=float, default=2.0,
                   help="give up after this long with no bytes (s)")
    p.add_argument("--deadline", type=float, default=18.0,
                   help="overall read budget (s)")
    args = p.parse_args(argv)

    has_end = probe(args.host, args.port, args.connect_timeout,
                    args.idle_timeout, args.deadline)
    return 0 if has_end else 1


if __name__ == "__main__":
    sys.exit(main())
