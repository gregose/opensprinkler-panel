#!/usr/bin/env python3
"""
Mock OpenSprinkler controller — enough of the local HTTP API to build and test
the panel firmware without the real controller.

Run:   python3 mock_os.py            # serves on 0.0.0.0:8080
Point the panel's os_host at this machine's IP:8080 (pw is not checked).

Implements: /jn (names+attrs), /jc (status), /js (light status),
            /cm (run/stop one station), /cv (stop all).
Faithfully models the "en=1 on an already-running station does nothing" quirk,
so off-then-on is required to change a station — exactly like real firmware.
"""
import json, time, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ---- configurable fake system ------------------------------------------------
NAMES = ["Front Lawn","Driveway Strip","North Beds","Back Lawn","Back Beds",
         "Patio Pots","Side Yard","Veg Garden","Rear Rotors","Parkway",
         "Front Rotors","Mailbox Bed","Pool Deck","Fence Line"]   # 14 stations
DISABLED = set()          # e.g. {5} to disable station index 5 (0-based)
MASTER   = set()          # sids that are master/pump stations (excluded from grid)
# ------------------------------------------------------------------------------

N = len(NAMES)
NBRD = (N + 7) // 8
_lock = threading.Lock()
# per-station runtime: None if off, else dict(start=epoch, dur=seconds)
_run = [None] * N

def _now(): return int(time.time())

def _tick():
    """Expire stations whose time is up (sequential: only one runs at a time)."""
    now = _now()
    for sid in range(N):
        r = _run[sid]
        if r and now >= r["start"] + r["dur"]:
            _run[sid] = None

def _board_bits(pred):
    bits = [0] * NBRD
    for sid in range(N):
        if pred(sid):
            bits[sid >> 3] |= (1 << (sid & 7))
    return bits

def jn():
    return {
        "masop":  _board_bits(lambda s: s in MASTER),
        "masop2": [0] * NBRD,
        "stn_dis":_board_bits(lambda s: s in DISABLED),
        "snames": NAMES,
        "maxlen": 32,
    }

def jc():
    _tick(); now = _now()
    sbits = _board_bits(lambda s: _run[s] is not None) + [0]
    ps = []
    for sid in range(N):
        r = _run[sid]
        if r:
            rem = max(0, r["start"] + r["dur"] - now)
            ps.append([99, rem, r["start"], 0])
        else:
            ps.append([0, 0, 0, 0])
    import random
    rssi = -60 - random.randint(0, 12)          # controller Wi-Fi RSSI, dBm
    return {"devt": now, "nbrd": NBRD, "en": 1, "sn1": 0, "sn2": 0,
            "rd": 0, "rdst": 0, "sbits": sbits, "ps": ps, "RSSI": rssi}

def js():
    _tick()
    sn = [1 if _run[s] is not None else 0 for s in range(N)]
    return {"sn": sn, "nstations": N}

def cm(q):
    """sid, en, t — mirrors firmware behavior including the re-en=1 no-op."""
    try:
        sid = int(q["sid"][0]); en = int(q["en"][0])
    except Exception:
        return {"result": 16}
    if sid < 0 or sid >= N:      return {"result": 17}
    if sid in MASTER:            return {"result": 32}   # not permitted
    with _lock:
        _tick()
        if en:
            try: t = int(q["t"][0])
            except Exception: return {"result": 16}
            if t < 1 or t > 64800: return {"result": 17}
            if _run[sid] is not None:
                return {"result": 1}        # QUIRK: already running -> no-op, no timer change
            # sequential: starting one implicitly is fine here; we don't queue others
            _run[sid] = {"start": _now(), "dur": t}
        else:
            _run[sid] = None
    return {"result": 1}

def cv(q):
    if q.get("rsn", ["0"])[0] == "1":
        with _lock:
            for s in range(N): _run[s] = None
    return {"result": 1}

ROUTES = {"/jn": lambda q: jn(), "/jc": lambda q: jc(), "/js": lambda q: js(),
          "/cm": cm, "/cv": cv}

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        fn = ROUTES.get(u.path)
        body = fn(q) if fn else {"result": 32}
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *a):  # quiet
        pass

if __name__ == "__main__":
    print(f"Mock OpenSprinkler on http://0.0.0.0:8080  ({N} stations)")
    ThreadingHTTPServer(("0.0.0.0", 8080), H).serve_forever()
