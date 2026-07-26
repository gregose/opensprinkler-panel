#!/usr/bin/env python3
"""
Mock OpenSprinkler controller — a faithful stand-in for the controller's local
HTTP API so the panel firmware (and humans) can be developed and manually tested
without the live sprinkler hardware.

Run:
    python3 docs/mock_os.py                 # serves on 0.0.0.0:8080
    python3 docs/mock_os.py --port 9090      # different port
    python3 docs/mock_os.py --schedule       # also fire programs at their start times
    python3 docs/mock_os.py --require-pw <md5>  # reject wrong pw with result:2

Point the panel's `os_host` at this machine's IP:8080 (pw is accepted but not
checked unless --require-pw is given).

Implements every endpoint the firmware uses (see docs/02-opensprinkler-api.md and
lib/os_client):

    GET /jn   station names + attributes (config, cached at startup)
    GET /jo   controller options (master station indices)
    GET /jc   controller status poll (devt, sbits, ps, RSSI, pause)
    GET /jp   program definitions (pd[] tuples)     [M9]
    GET /js   light status (sn[], nstations)
    GET /cm   run/stop one station (sid,en,t[,ssta])
    GET /cv   stop all (rsn=1)
    GET /mp   run a program now (pid, 0-based)       [M9]  -> reports pid 254
    GET /cp   enable/disable a program (pid,en)       [M9]
    GET /pq   pause / resume programs (dur)           [M9]

Debug helpers (not part of the real API, prefixed with an underscore):

    GET /_reset            reset all runtime state
    GET /_run?pid=N        launch program N as a *scheduled* run (real pid=N+1),
                           so the "scheduled program identification" path can be
                           exercised deterministically without a wall clock.
    GET /_state            dump internal queue/pause state as JSON

Faithful behaviours modelled:
  * Sequential stations — only one station runs at a time; a program enqueues its
    stations and they run back-to-back.
  * The "en=1 on an already-running station is a NO-OP" quirk — so advance/extend
    must off-then-on, exactly like the real firmware.
  * A manual program run (/mp) reports pid 254 in /jc (no program index); a
    *scheduled* run (/_run or --schedule) reports the 1-based program id. This is
    what makes the panel identify the running program by its station set.
  * Completed stations drop out of ps[] (pid 0); upcoming queued stations carry a
    future `start` and their full duration in `rem`.
  * devt is controller *local* wall-clock epoch (timezone-adjusted), matching a
    real controller, so the panel's next-run day/time math lines up.
  * Pause (/pq) freezes the running countdown and reports pq=1 + pt seconds.
"""

from __future__ import annotations

import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ---------------------------------------------------------------------------
# Result codes (docs/02 §Authentication)
# ---------------------------------------------------------------------------
R_OK = 1
R_UNAUTHORIZED = 2
R_MISMATCH = 3
R_DATA_MISSING = 16
R_OUT_OF_RANGE = 17
R_NOT_PERMITTED = 32

# ---------------------------------------------------------------------------
# /jp program-tuple flag bits (mirror lib/program_model load_program)
# ---------------------------------------------------------------------------
FLAG_ENABLED = 1 << 0
FLAG_WEATHER = 1 << 1
FLAG_STARTTIME_FIXED = 1 << 6
FLAG_DATERANGE = 1 << 7
# bits 2-3 = oddeven, bits 4-5 = ProgramType (0 = Weekly)

# Weekday bitmask helpers — the firmware uses wd = (sun0 + 6) % 7, i.e. Monday=0.
MON, TUE, WED, THU, FRI, SAT, SUN = (1 << i for i in range(7))
EVERY_DAY = 0x7F


def local_epoch() -> int:
    """Wall-clock epoch in the machine's local timezone (like a real OS devt)."""
    t = time.time()
    return int(t + time.localtime(t).tm_gmtoff)


def fixed_start(minutes: int) -> list[int]:
    """Encode a single fixed daily start time (minutes since midnight)."""
    return [minutes, -1, -1, -1]  # -1 slots are 'disabled' (bit15 set)


# ---------------------------------------------------------------------------
# Default fake system. Programs are chosen to have DISTINCT station sets so the
# panel's "identify the running program by its stations" logic can be validated.
# ---------------------------------------------------------------------------
#
# 24 stations (a realistic 3-expansion-board config, nbrd=3). Station 14 has a
# deliberately LONG name so the queue-row text-wrapping/ellipsis behaviour can be
# exercised on-device; it belongs to the enabled "Full System Test" program below
# so it actually shows up in a running queue.
DEFAULT_NAMES = [
    "Front Lawn", "Driveway Strip", "North Beds", "Back Lawn", "Back Beds",
    "Patio Pots", "Side Yard", "Veg Garden", "Rear Rotors", "Parkway",
    "Front Rotors", "Mailbox Bed", "Pool Deck", "Fence Line",
    "Northeast Perimeter Drip Line Extension",  # 14 — intentionally long
    "Orchard Row", "Greenhouse Mist", "Raised Beds", "Herb Spiral",
    "Compost Corner", "Rain Garden", "Swale Line", "Berry Patch",
    "Cutting Garden",
]  # 24 stations


class ProgramDef:
    """A program as the mock stores it; serialised into a /jp `pd` tuple."""

    def __init__(self, name, enabled, days0, start_min, durations):
        self.name = name
        self.enabled = enabled
        self.days0 = days0          # weekday bitmask (Weekly type)
        self.start_min = start_min  # minutes since local midnight
        self.durations = durations  # per-station seconds (len == station count)

    def flag(self) -> int:
        f = FLAG_STARTTIME_FIXED  # Weekly (bits 4-5 = 0), fixed daily start
        if self.enabled:
            f |= FLAG_ENABLED
        return f

    def pd_tuple(self) -> list:
        # [flag, days0, days1, [start x4], [dur xN], name, [endr, from, to]]
        return [
            self.flag(),
            self.days0,
            0,
            fixed_start(self.start_min),
            list(self.durations),
            self.name,
            [0, 0, 0],
        ]

    def station_sids(self) -> list[int]:
        return [i for i, d in enumerate(self.durations) if d > 0]


def default_programs(n_stations: int) -> list[ProgramDef]:
    """Programs for exercising the on-device Programs UI end-to-end.

      - 10 programs total -> THREE list pages (MAX_PROG_ROWS == 4), so the
        pager dots + ‹ › arrows have to render and page 1<->2<->3.
      - "Full System Test" has 11 stations -> overflows the queue window
        (MAX_QROWS == 9) so windowing + fade indicators show.
      - that program also contains station 14 (the long name) -> queue-row
        text wrapping/ellipsis is exercised while it runs.
      - one program ("Northeast Perimeter & Back Forty Seasonal Deep-Soak
        Cycle") has an intentionally long NAME -> the programs-list name column
        must ellipsize it.

    The ENABLED programs keep DISJOINT station sets so the panel's "identify the
    running program by its live stations" matcher stays unambiguous (that matcher
    only ever runs for programs the controller can actually start, i.e. enabled
    ones). Disabled programs are list-only padding and may reuse stations.
    """

    def durs(mapping: dict[int, int]) -> list[int]:
        v = [0] * n_stations
        for sid, secs in mapping.items():
            if 0 <= sid < n_stations:
                v[sid] = secs
        return v

    return [
        # Morning Lawn — every day 06:00 — lawns + rotors
        ProgramDef("Morning Lawn", True, EVERY_DAY, 6 * 60,
                   durs({0: 600, 3: 600, 8: 900})),
        # Garden Drip — Mon/Wed/Fri 05:30 — beds + veg
        ProgramDef("Garden Drip", True, MON | WED | FRI, 5 * 60 + 30,
                   durs({2: 300, 4: 300, 7: 420})),
        # Evening Patio — DISABLED — 18:30 — pots + pool deck
        ProgramDef("Evening Patio", False, EVERY_DAY, 18 * 60 + 30,
                   durs({5: 120, 12: 180})),
        # Full System Test — every day 04:00 — 11 stations, overflows the
        # queue window; includes the long-named station 14.
        ProgramDef("Full System Test", True, EVERY_DAY, 4 * 60,
                   durs({1: 300, 6: 240, 9: 360, 10: 300, 11: 180, 13: 240,
                         14: 600, 15: 300, 16: 420, 17: 240, 18: 180})),
        # Backyard Soak — Tue/Thu 07:00 — deep soak beds
        ProgramDef("Backyard Soak", True, TUE | THU, 7 * 60,
                   durs({19: 900, 20: 720})),
        # Front Curb Strip — DISABLED — Sat/Sun 06:30 — curb strips
        ProgramDef("Front Curb Strip", False, SAT | SUN, 6 * 60 + 30,
                   durs({21: 300, 22: 240})),
        # Northeast Perimeter ... — ENABLED — long NAME for the programs-list
        # ellipsis test; uses only the one otherwise-unused station (23) so the
        # enabled set stays disjoint.
        ProgramDef("Northeast Perimeter & Back Forty Seasonal Deep-Soak Cycle",
                   True, MON | THU, 3 * 60, durs({23: 600})),
        # --- list-only padding so the list spans three pages (pager test). ---
        # DISABLED, so they never run -> reusing stations is unambiguous.
        ProgramDef("Holiday Away Mode", False, EVERY_DAY, 20 * 60,
                   durs({0: 300, 3: 300})),
        ProgramDef("Deep Root Quarterly", False, SUN, 9 * 60,
                   durs({8: 1200, 9: 1200})),
        ProgramDef("Overseed Germination", False, MON | WED | FRI, 5 * 60,
                   durs({2: 180, 4: 180, 7: 180})),
    ]


# ---------------------------------------------------------------------------
# The controller model — all state lives here so tests can spin up an isolated
# instance without touching module globals.
# ---------------------------------------------------------------------------
class MockController:
    def __init__(self, names=None, disabled=None, master=None,
                 sunrise_min=6 * 60, sunset_min=20 * 60, require_pw=None,
                 programs=None):
        self.names = list(names) if names is not None else list(DEFAULT_NAMES)
        self.n = len(self.names)
        self.nbrd = (self.n + 7) // 8
        self.disabled = set(disabled or ())
        self.master = set(master or ())      # 0-based sids that are master valves
        self.sunrise_min = sunrise_min
        self.sunset_min = sunset_min
        self.require_pw = require_pw
        self.programs = programs if programs is not None else default_programs(self.n)

        self._lock = threading.RLock()
        # queue: ordered list of dicts {sid, total, rem, pid}. Index 0 is the
        # running head; the rest are upcoming (sequential).
        self._queue: list[dict] = []
        self._last_tick = local_epoch()
        self._paused = False
        self._pause_deadline = 0
        self._sched_fired: dict[int, int] = {}  # pid -> minute-of-day last fired

    # -- internal helpers ----------------------------------------------------
    def _tick(self):
        """Advance the sequential run queue; call under lock before any read."""
        now = local_epoch()
        dt = now - self._last_tick
        self._last_tick = now
        if dt < 0:
            dt = 0

        if self._paused:
            if now >= self._pause_deadline:
                self._paused = False  # auto-resume; do not burn this dt
            return

        while self._queue and dt > 0:
            head = self._queue[0]
            if head["rem"] <= dt:
                dt -= head["rem"]
                head["rem"] = 0
                self._queue.pop(0)  # completed -> drops out of ps[]
            else:
                head["rem"] -= dt
                dt = 0

    def _board_bits(self, on_sids: set[int]) -> list[int]:
        bits = [0] * self.nbrd
        for sid in on_sids:
            bits[sid >> 3] |= 1 << (sid & 7)
        return bits

    def _check_pw(self, q) -> bool:
        if self.require_pw is None:
            return True
        return q.get("pw", [""])[0] == self.require_pw

    def _start_program(self, prog: ProgramDef, pid_value: int):
        """Replace the queue with this program's stations (sequential)."""
        self._queue = [
            {"sid": sid, "total": prog.durations[sid],
             "rem": prog.durations[sid], "pid": pid_value}
            for sid in prog.station_sids()
        ]
        self._paused = False
        self._last_tick = local_epoch()

    # -- GET /jn -------------------------------------------------------------
    def jn(self) -> dict:
        return {
            "masop": self._board_bits(self.master),
            "masop2": [0] * self.nbrd,
            "stn_dis": self._board_bits(self.disabled),
            "snames": self.names,
            "maxlen": 32,
        }

    # -- GET /jo -------------------------------------------------------------
    def jo(self) -> dict:
        mas = (min(self.master) + 1) if self.master else 0  # 1-based; 0 = none
        return {"mas": mas, "mas2": 0}

    # -- GET /jc -------------------------------------------------------------
    def jc(self) -> dict:
        with self._lock:
            self._maybe_schedule()
            self._tick()
            now = local_epoch()

            ps = [[0, 0, 0, 0] for _ in range(self.n)]
            on_sids: set[int] = set()
            # Running head: on now, start in the past, rem counting down.
            offset = 0  # seconds until each queued entry starts
            for i, e in enumerate(self._queue):
                sid = e["sid"]
                if i == 0 and not self._paused:
                    elapsed = e["total"] - e["rem"]
                    ps[sid] = [e["pid"], e["rem"], now - elapsed, 0]
                    on_sids.add(sid)
                    offset = e["rem"]
                elif i == 0 and self._paused:
                    # Frozen: still "the current station" but reported as queued
                    # at now so the panel shows the frozen remaining.
                    ps[sid] = [e["pid"], e["rem"], now, 0]
                    on_sids.add(sid)
                    offset = e["rem"]
                else:
                    # Upcoming: future start, full duration in rem.
                    ps[sid] = [e["pid"], e["total"], now + offset, 0]
                    offset += e["total"]

            sbits = self._board_bits(on_sids) + [0]
            pt = max(0, self._pause_deadline - now) if self._paused else 0
            rssi = -62 - (int(now) % 11)  # deterministic-ish controller RSSI
            return {
                "devt": now, "nbrd": self.nbrd, "en": 1, "sn1": 0, "sn2": 0,
                "rd": 0, "rdst": 0, "sunrise": self.sunrise_min,
                "sunset": self.sunset_min, "pq": 1 if self._paused else 0,
                "pt": pt, "sbits": sbits, "ps": ps, "RSSI": rssi,
            }

    # -- GET /js -------------------------------------------------------------
    def js(self) -> dict:
        with self._lock:
            self._tick()
            on = {self._queue[0]["sid"]} if (self._queue and not self._paused) else set()
            sn = [1 if s in on else 0 for s in range(self.n)]
            return {"sn": sn, "nstations": self.n}

    # -- GET /jp -------------------------------------------------------------
    def jp(self) -> dict:
        with self._lock:
            return {
                "nprogs": len(self.programs),
                "nboards": self.nbrd,
                "mnp": 40, "mnst": 4, "pnsize": 32,
                "pd": [p.pd_tuple() for p in self.programs],
            }

    # -- GET /cm -------------------------------------------------------------
    def cm(self, q) -> dict:
        if not self._check_pw(q):
            return {"result": R_UNAUTHORIZED}
        try:
            sid = int(q["sid"][0])
            en = int(q["en"][0])
        except (KeyError, ValueError, IndexError):
            return {"result": R_DATA_MISSING}
        if sid < 0 or sid >= self.n:
            return {"result": R_OUT_OF_RANGE}
        if sid in self.master:
            return {"result": R_NOT_PERMITTED}

        skip = q.get("ssta", ["0"])[0] == "1"
        with self._lock:
            self._tick()
            if en:
                try:
                    t = int(q["t"][0])
                except (KeyError, ValueError, IndexError):
                    return {"result": R_DATA_MISSING}
                if t < 1 or t > 64800:
                    return {"result": R_OUT_OF_RANGE}
                # QUIRK: en=1 on the already-running station changes nothing.
                if self._queue and self._queue[0]["sid"] == sid and not self._paused:
                    return {"result": R_OK}
                # Manual single-station run -> pid 99, replaces the queue.
                self._queue = [{"sid": sid, "total": t, "rem": t, "pid": 99}]
                self._paused = False
                self._last_tick = local_epoch()
            else:
                if skip:
                    # Skip-advance: drop the head, next station takes over.
                    if self._queue:
                        self._queue.pop(0)
                        self._last_tick = local_epoch()
                else:
                    # Stop just this station (remove from queue if present).
                    self._queue = [e for e in self._queue if e["sid"] != sid]
                    self._last_tick = local_epoch()
        return {"result": R_OK}

    # -- GET /cv -------------------------------------------------------------
    def cv(self, q) -> dict:
        if not self._check_pw(q):
            return {"result": R_UNAUTHORIZED}
        if q.get("rsn", ["0"])[0] == "1":
            with self._lock:
                self._queue = []
                self._paused = False
        return {"result": R_OK}

    # -- GET /mp (manual program run -> pid 254) -----------------------------
    def mp(self, q) -> dict:
        if not self._check_pw(q):
            return {"result": R_UNAUTHORIZED}
        try:
            pid = int(q["pid"][0])  # 0-based program index
        except (KeyError, ValueError, IndexError):
            return {"result": R_DATA_MISSING}
        if pid < 0 or pid >= len(self.programs):
            return {"result": R_OUT_OF_RANGE}
        prog = self.programs[pid]
        if not prog.station_sids():
            return {"result": R_OK}  # nothing to run, but not an error
        with self._lock:
            self._tick()
            self._start_program(prog, pid_value=254)  # manual run reports 254
        return {"result": R_OK}

    # -- GET /cp (enable/disable a program) ----------------------------------
    def cp(self, q) -> dict:
        if not self._check_pw(q):
            return {"result": R_UNAUTHORIZED}
        try:
            pid = int(q["pid"][0])  # 0-based program index
            en = int(q["en"][0])
        except (KeyError, ValueError, IndexError):
            return {"result": R_DATA_MISSING}
        if pid < 0 or pid >= len(self.programs):
            return {"result": R_OUT_OF_RANGE}
        with self._lock:
            self.programs[pid].enabled = bool(en)
        return {"result": R_OK}

    # -- GET /pq (pause / resume) --------------------------------------------
    def pq(self, q) -> dict:
        if not self._check_pw(q):
            return {"result": R_UNAUTHORIZED}
        try:
            dur = int(q["dur"][0])
        except (KeyError, ValueError, IndexError):
            dur = 600
        with self._lock:
            self._tick()
            if self._paused:
                self._paused = False  # any /pq while paused cancels the pause
            else:
                self._paused = True
                self._pause_deadline = local_epoch() + max(1, dur)
        return {"result": R_OK}

    # -- scheduler (opt-in) --------------------------------------------------
    def enable_scheduler(self):
        self._scheduler_on = True

    def _maybe_schedule(self):
        if not getattr(self, "_scheduler_on", False):
            return
        if self._queue:  # something already running; don't stomp it
            return
        now = local_epoch()
        minute_of_day = (now // 60) % (24 * 60)
        wd = (time.gmtime(now).tm_wday)  # Monday=0 already
        for i, p in enumerate(self.programs):
            if not p.enabled:
                continue
            if not (p.days0 & (1 << wd)):
                continue
            if p.start_min != minute_of_day:
                continue
            if self._sched_fired.get(i) == minute_of_day:
                continue
            self._sched_fired[i] = minute_of_day
            self._start_program(p, pid_value=i + 1)  # scheduled -> real pid
            break

    # -- debug helpers -------------------------------------------------------
    def dbg_run(self, q) -> dict:
        """Force a *scheduled-style* program run (real pid = index+1)."""
        try:
            pid = int(q["pid"][0])
        except (KeyError, ValueError, IndexError):
            return {"result": R_DATA_MISSING}
        if pid < 0 or pid >= len(self.programs):
            return {"result": R_OUT_OF_RANGE}
        with self._lock:
            self._tick()
            self._start_program(self.programs[pid], pid_value=pid + 1)
        return {"result": R_OK}

    def dbg_reset(self, q) -> dict:
        with self._lock:
            self._queue = []
            self._paused = False
            self._sched_fired = {}
            self.programs = default_programs(self.n)
        return {"result": R_OK}

    def dbg_state(self, q) -> dict:
        with self._lock:
            self._tick()
            return {
                "queue": list(self._queue),
                "paused": self._paused,
                "pause_remaining": max(0, self._pause_deadline - local_epoch())
                if self._paused else 0,
                "programs": [{"name": p.name, "enabled": p.enabled,
                              "stations": p.station_sids()} for p in self.programs],
            }

    # -- dispatch ------------------------------------------------------------
    def handle(self, path: str, q: dict) -> dict:
        routes = {
            "/jn": lambda: self.jn(),
            "/jo": lambda: self.jo(),
            "/jc": lambda: self.jc(),
            "/js": lambda: self.js(),
            "/jp": lambda: self.jp(),
            "/cm": lambda: self.cm(q),
            "/cv": lambda: self.cv(q),
            "/mp": lambda: self.mp(q),
            "/cp": lambda: self.cp(q),
            "/pq": lambda: self.pq(q),
            "/_run": lambda: self.dbg_run(q),
            "/_reset": lambda: self.dbg_reset(q),
            "/_state": lambda: self.dbg_state(q),
        }
        fn = routes.get(path)
        return fn() if fn else {"result": R_NOT_PERMITTED}


# ---------------------------------------------------------------------------
# HTTP wiring
# ---------------------------------------------------------------------------
def make_handler(ctrl: MockController):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            body = ctrl.handle(u.path, q)
            data = json.dumps(body).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, *a):  # quiet by default
            pass

    return Handler


def build_server(host: str, port: int, ctrl: MockController) -> ThreadingHTTPServer:
    return ThreadingHTTPServer((host, port), make_handler(ctrl))


def main():
    ap = argparse.ArgumentParser(description="Mock OpenSprinkler controller")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--stations", type=int, default=len(DEFAULT_NAMES),
                    help="number of stations (uses/truncates the default names)")
    ap.add_argument("--sunrise", type=int, default=6 * 60,
                    help="sunrise, minutes since local midnight")
    ap.add_argument("--sunset", type=int, default=20 * 60,
                    help="sunset, minutes since local midnight")
    ap.add_argument("--require-pw", default=None, metavar="MD5",
                    help="reject requests whose pw != this md5 (tests auth)")
    ap.add_argument("--schedule", action="store_true",
                    help="fire programs at their start times (real pid)")
    args = ap.parse_args()

    names = DEFAULT_NAMES[:args.stations]
    while len(names) < args.stations:  # pad if asked for more than we have names
        names.append(f"Station {len(names) + 1}")
    ctrl = MockController(names=names, sunrise_min=args.sunrise,
                          sunset_min=args.sunset, require_pw=args.require_pw)
    if args.schedule:
        ctrl.enable_scheduler()

    srv = build_server(args.host, args.port, ctrl)
    print(f"Mock OpenSprinkler on http://{args.host}:{args.port}  "
          f"({ctrl.n} stations, {len(ctrl.programs)} programs"
          f"{', scheduler ON' if args.schedule else ''})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
        srv.shutdown()


if __name__ == "__main__":
    main()
