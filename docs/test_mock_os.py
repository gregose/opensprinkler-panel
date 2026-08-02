#!/usr/bin/env python3
"""
Connection + API-contract tests for the mock OpenSprinkler controller.

These are the "connection tests" that let you trust the mock as a manual-testing
stand-in: they boot the mock on an ephemeral local port and exercise every
endpoint the panel firmware uses, asserting the exact JSON shape and the tricky
behaviours (manual and program queues, en=1 no-op, pid 254 vs real pid, pause,
enable/disable).

Run:
    python3 docs/test_mock_os.py            # verbose unittest run
    python3 -m unittest docs.test_mock_os   # via unittest discovery

Pure standard library - no pytest / third-party deps.
"""

import json
import threading
import unittest
import urllib.request
from urllib.parse import urlencode

import mock_os
from mock_os import (
    FLAG_ENABLED, MockController, build_server, default_programs,
    DEFAULT_NAMES, MOCK_CURRENT_MA,
)

# On-device UI capacities the default fixture is designed to exceed. Keep these
# in sync with src/main.cpp (MAX_PROG_ROWS, MAX_QROWS).
PANEL_MAX_PROG_ROWS = 4   # programs list rows per page
PANEL_MAX_QROWS = 9       # visible queue rows on the program-run screen
QUEUE_NAME_WRAP_CHARS = 16  # ~chars that fit a queue row name before ellipsis


class MockServerCase(unittest.TestCase):
    """Base class: boots an isolated mock server per test on 127.0.0.1:0."""

    def setUp(self):
        self.ctrl = MockController()
        self.srv = build_server("127.0.0.1", 0, self.ctrl)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=2)

    # -- helpers -------------------------------------------------------------
    def get(self, path, **params):
        url = f"http://127.0.0.1:{self.port}{path}"
        if params:
            url += "?" + urlencode(params)
        with urllib.request.urlopen(url, timeout=2) as resp:
            self.assertEqual(resp.status, 200)
            return json.loads(resp.read().decode())


class ConnectionTests(MockServerCase):
    def test_server_reachable(self):
        # The most basic connection test: /jc must answer with a devt.
        jc = self.get("/jc")
        self.assertIn("devt", jc)
        self.assertGreater(jc["devt"], 0)

    def test_unknown_path_returns_not_permitted(self):
        self.assertEqual(self.get("/nope")["result"], mock_os.R_NOT_PERMITTED)

    def test_client_style_boot_then_poll(self):
        # Mirrors the firmware boot: /jn once, then a /jc poll.
        jn = self.get("/jn")
        self.assertEqual(len(jn["snames"]), self.ctrl.n)
        jc = self.get("/jc")
        self.assertEqual(len(jc["ps"]), self.ctrl.n)


class JnJoTests(MockServerCase):
    def test_jn_shape(self):
        jn = self.get("/jn")
        self.assertEqual(jn["snames"], self.ctrl.names)
        self.assertEqual(len(jn["stn_dis"]), self.ctrl.nbrd)
        self.assertEqual(len(jn["masop"]), self.ctrl.nbrd)
        self.assertEqual(len(jn["masop2"]), self.ctrl.nbrd)
        self.assertEqual(jn["maxlen"], 32)

    def test_jo_master_indices(self):
        jo = self.get("/jo")
        self.assertIn("mas", jo)
        self.assertIn("mas2", jo)
        self.assertEqual(jo["mas"], 0)  # no master by default


class JcTests(MockServerCase):
    def test_idle_has_all_zero_ps(self):
        jc = self.get("/jc")
        self.assertTrue(all(e == [0, 0, 0, 0] for e in jc["ps"]))
        self.assertEqual(jc["pq"], 0)
        self.assertEqual(jc["curr"], 0)

    def test_rssi_is_negative(self):
        self.assertLess(self.get("/jc")["RSSI"], 0)

    def test_sunrise_sunset_present(self):
        jc = self.get("/jc")
        self.assertEqual(jc["sunrise"], self.ctrl.sunrise_min)
        self.assertEqual(jc["sunset"], self.ctrl.sunset_min)


class JpTests(MockServerCase):
    def test_jp_shape(self):
        jp = self.get("/jp")
        self.assertEqual(jp["nprogs"], len(self.ctrl.programs))
        self.assertEqual(len(jp["pd"]), len(self.ctrl.programs))
        for row in jp["pd"]:
            self.assertEqual(len(row), 7)              # tuple arity
            self.assertEqual(len(row[3]), 4)           # 4 start slots
            self.assertEqual(len(row[4]), self.ctrl.n) # per-station durations
            self.assertIsInstance(row[5], str)         # name

    def test_flag_reflects_enabled(self):
        jp = self.get("/jp")
        for row, prog in zip(jp["pd"], self.ctrl.programs):
            self.assertEqual(bool(row[0] & FLAG_ENABLED), prog.enabled)

    def test_third_program_disabled_by_default(self):
        self.assertFalse(self.get("/jp")["pd"][2][0] & FLAG_ENABLED)


class JlTests(MockServerCase):
    def test_jl_realistic_oldest_first_mix(self):
        rows = self.get("/jl", hist=30)
        self.assertIsInstance(rows, list)
        self.assertEqual(rows, sorted(rows, key=lambda row: row[3]))
        self.assertTrue(all(len(row) in (4, 5) for row in rows))
        self.assertTrue(any(len(row) == 4 for row in rows))
        self.assertTrue(any(len(row) == 5 for row in rows))
        self.assertTrue(any(row[0] == 99 for row in rows))
        self.assertTrue(any(row[0] == 254 for row in rows))
        self.assertTrue(any(row[:2] == [0, "rd"] for row in rows))
        self.assertTrue(any(row[:2] == [0, "s1"] for row in rows))

    def test_jl_hist_uses_controller_local_calendar_days(self):
        today = (mock_os.local_epoch() // 86400) * 86400
        self.ctrl.history = [
            [1, 0, 10, today - 1],
            [1, 1, 20, today + 1],
        ]
        self.assertEqual(self.get("/jl", hist=0), [[1, 1, 20, today + 1]])
        self.assertEqual(self.get("/jl", hist=1), self.ctrl.history)

    def test_jl_start_end_are_inclusive(self):
        self.ctrl.history = [
            [1, 0, 10, 100],
            [1, 1, 20, 200],
            [1, 2, 30, 300],
        ]
        self.assertEqual(
            self.get("/jl", start=100, end=200),
            self.ctrl.history[:2],
        )

    def test_jl_rejects_incomplete_or_oversized_ranges(self):
        self.assertEqual(
            self.get("/jl", start=100)["result"], mock_os.R_DATA_MISSING
        )
        self.assertEqual(
            self.get("/jl", start=0, end=366 * 86400)["result"],
            mock_os.R_OUT_OF_RANGE,
        )
        self.assertEqual(
            self.get("/jl", hist=366)["result"], mock_os.R_OUT_OF_RANGE
        )

    def test_jl_type_filters_special_events(self):
        rows = self.get("/jl", hist=30, type="rd")
        self.assertTrue(rows)
        self.assertTrue(all(row[:2] == [0, "rd"] for row in rows))

    def test_jl_honors_required_password(self):
        self.ctrl.require_pw = "secret"
        self.assertEqual(
            self.get("/jl", hist=30)["result"], mock_os.R_UNAUTHORIZED
        )
        self.assertIsInstance(self.get("/jl", pw="secret", hist=30), list)


class CmStationTests(MockServerCase):
    def _running_sid(self, jc):
        for sid, e in enumerate(jc["ps"]):
            if e[0] != 0 and e[1] > 0:
                return sid
        return None

    def _is_on(self, jc, sid):
        return bool(jc["sbits"][sid >> 3] & (1 << (sid & 7)))

    def test_run_station_turns_it_on(self):
        self.assertEqual(self.get("/cm", sid=0, en=1, t=120)["result"], 1)
        jc = self.get("/jc")
        self.assertEqual(jc["ps"][0][0], 99)      # manual pid
        self.assertGreater(jc["ps"][0][1], 0)     # rem > 0
        self.assertEqual(self._running_sid(jc), 0)
        self.assertEqual(jc["curr"], MOCK_CURRENT_MA)

    def test_en1_on_running_station_is_noop(self):
        self.get("/cm", sid=0, en=1, t=120)
        rem1 = self.get("/jc")["ps"][0][1]
        # Re-issue en=1 with a *different* duration; the quirk means no change.
        self.assertEqual(self.get("/cm", sid=0, en=1, t=600)["result"], 1)
        rem2 = self.get("/jc")["ps"][0][1]
        self.assertLessEqual(rem2, rem1)          # counting down, not reset to 600
        self.assertLess(rem2, 300)                # definitely not the new 600

    def test_manual_runs_append_in_order(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/cm", sid=2, en=1, t=240)

        jc = self.get("/jc")
        self.assertEqual(jc["ps"][1][0], 99)
        self.assertEqual(jc["ps"][2][0], 99)
        self.assertLessEqual(jc["ps"][1][2], jc["devt"])
        self.assertGreater(jc["ps"][1][1], 0)
        self.assertLessEqual(jc["ps"][1][1], 120)
        self.assertTrue(self._is_on(jc, 1))
        self.assertEqual(jc["ps"][2][1], 240)
        self.assertGreater(jc["ps"][2][2], jc["devt"])
        self.assertFalse(self._is_on(jc, 2))

        state = self.get("/_state")
        self.assertEqual([e["sid"] for e in state["queue"]], [1, 2])
        self.assertEqual([e["pid"] for e in state["queue"]], [99, 99])

    def test_duplicate_manual_append_is_noop(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/cm", sid=2, en=1, t=240)
        self.assertEqual(self.get("/cm", sid=1, en=1, t=600)["result"], 1)

        queue = self.get("/_state")["queue"]
        self.assertEqual([e["sid"] for e in queue], [1, 2])
        self.assertEqual(len(queue), 2)
        self.assertEqual(queue[0]["total"], 120)

    def test_skip_advance_drains_manual_queue_head(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/cm", sid=2, en=1, t=240)
        self.assertEqual(self.get("/cm", sid=1, en=0, ssta=1)["result"], 1)

        jc = self.get("/jc")
        self.assertEqual(jc["ps"][1], [0, 0, 0, 0])
        self.assertEqual(jc["ps"][2][0], 99)
        self.assertGreater(jc["ps"][2][1], 0)
        self.assertLessEqual(jc["ps"][2][1], 240)
        self.assertLessEqual(jc["ps"][2][2], jc["devt"])
        self.assertTrue(self._is_on(jc, 2))

    def test_cv_stops_manual_queue(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/cm", sid=2, en=1, t=240)
        self.assertEqual(self.get("/cv", rsn=1)["result"], 1)

        jc = self.get("/jc")
        self.assertTrue(all(e == [0, 0, 0, 0] for e in jc["ps"]))
        self.assertEqual(self.get("/_state")["queue"], [])

    def test_manual_append_does_not_resume_paused_queue(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/pq", dur=300)
        self.get("/cm", sid=2, en=1, t=240)

        jc = self.get("/jc")
        self.assertEqual(jc["pq"], 1)
        queue = self.get("/_state")["queue"]
        self.assertEqual([e["sid"] for e in queue], [1, 2])

    def test_stop_station(self):
        self.get("/cm", sid=0, en=1, t=120)
        self.assertEqual(self.get("/cm", sid=0, en=0)["result"], 1)
        self.assertIsNone(self._running_sid(self.get("/jc")))

    def test_out_of_range_sid(self):
        self.assertEqual(self.get("/cm", sid=999, en=1, t=60)["result"],
                         mock_os.R_OUT_OF_RANGE)

    def test_missing_params(self):
        self.assertEqual(self.get("/cm", sid=0)["result"], mock_os.R_DATA_MISSING)

    def test_bad_duration(self):
        self.assertEqual(self.get("/cm", sid=0, en=1, t=0)["result"],
                         mock_os.R_OUT_OF_RANGE)

    def test_cv_stops_everything(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.assertEqual(self.get("/cv", rsn=1)["result"], 1)
        jc = self.get("/jc")
        self.assertTrue(all(e[0] == 0 for e in jc["ps"]))


class ProgramRunTests(MockServerCase):
    def test_mp_reports_pid_254_and_program_stations(self):
        # /mp is what the panel's Run button calls -> manual run reports pid 254.
        self.assertEqual(self.get("/mp", pid=0)["result"], 1)
        jc = self.get("/jc")
        prog0 = self.ctrl.programs[0]
        pids = {e[0] for e in jc["ps"] if e[0] != 0}
        self.assertEqual(pids, {254})
        # Exactly the program's stations are in the queue.
        queued = {sid for sid, e in enumerate(jc["ps"]) if e[0] == 254}
        self.assertEqual(queued, set(prog0.station_sids()))

    def test_mp_replaces_manual_queue(self):
        self.get("/cm", sid=1, en=1, t=120)
        self.get("/cm", sid=2, en=1, t=240)
        self.assertEqual(self.get("/mp", pid=0)["result"], 1)

        state = self.get("/_state")
        self.assertTrue(state["queue"])
        self.assertTrue(all(e["pid"] == 254 for e in state["queue"]))
        self.assertFalse(any(e["pid"] == 99 for e in state["queue"]))

    def test_program_runs_sequentially(self):
        self.get("/mp", pid=0)
        jc = self.get("/jc")
        prog0 = self.ctrl.programs[0]
        sids = prog0.station_sids()
        # First station is on (start <= devt); the rest are queued in the future.
        first = sids[0]
        self.assertEqual(self.get("/jc")["ps"][first][0], 254)
        self.assertLessEqual(jc["ps"][first][2], jc["devt"])
        for later in sids[1:]:
            self.assertGreater(jc["ps"][later][2], jc["devt"])  # future start
            self.assertEqual(jc["ps"][later][1], prog0.durations[later])  # full dur

    def test_debug_run_reports_real_pid(self):
        # /_run simulates a *scheduled* run -> real 1-based pid (index+1).
        self.assertEqual(self.get("/_run", pid=1)["result"], 1)
        jc = self.get("/jc")
        pids = {e[0] for e in jc["ps"] if e[0] != 0}
        self.assertEqual(pids, {2})  # program index 1 -> pid 2

    def test_skip_advance_moves_to_next_station(self):
        self.get("/mp", pid=0)
        sids = self.ctrl.programs[0].station_sids()
        first, second = sids[0], sids[1]
        # ssta=1 skip on the running head advances the program to the next.
        self.assertEqual(self.get("/cm", sid=first, en=0, ssta=1)["result"], 1)
        jc = self.get("/jc")
        self.assertEqual(jc["ps"][first][0], 0)          # first dropped out
        self.assertLessEqual(jc["ps"][second][2], jc["devt"])  # second now running


class ProgramEnableTests(MockServerCase):
    def test_cp_disable_then_enable(self):
        # Program 0 is enabled by default.
        self.assertTrue(self.get("/jp")["pd"][0][0] & FLAG_ENABLED)
        self.assertEqual(self.get("/cp", pid=0, en=0)["result"], 1)
        self.assertFalse(self.get("/jp")["pd"][0][0] & FLAG_ENABLED)
        self.assertEqual(self.get("/cp", pid=0, en=1)["result"], 1)
        self.assertTrue(self.get("/jp")["pd"][0][0] & FLAG_ENABLED)

    def test_cp_targets_only_the_named_program(self):
        # Regression guard for the "enabling P2 toggled P3" off-by-one bug:
        # toggling one 0-based index must not disturb its neighbours.
        self.get("/cp", pid=1, en=0)
        pd = self.get("/jp")["pd"]
        self.assertTrue(pd[0][0] & FLAG_ENABLED)   # unchanged
        self.assertFalse(pd[1][0] & FLAG_ENABLED)  # the target
        self.assertFalse(pd[2][0] & FLAG_ENABLED)  # was already disabled

    def test_cp_out_of_range(self):
        self.assertEqual(self.get("/cp", pid=99, en=1)["result"],
                         mock_os.R_OUT_OF_RANGE)


class PauseTests(MockServerCase):
    def test_pause_sets_flags_and_resume_clears(self):
        self.get("/mp", pid=0)
        self.assertEqual(self.get("/pq", dur=300)["result"], 1)
        jc = self.get("/jc")
        self.assertEqual(jc["pq"], 1)
        self.assertGreater(jc["pt"], 0)
        # A second /pq cancels the pause.
        self.assertEqual(self.get("/pq", dur=300)["result"], 1)
        self.assertEqual(self.get("/jc")["pq"], 0)


class AuthTests(unittest.TestCase):
    def setUp(self):
        self.ctrl = MockController(require_pw="deadbeef")
        self.srv = build_server("127.0.0.1", 0, self.ctrl)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        self.thread.join(timeout=2)

    def _get(self, path, **params):
        url = f"http://127.0.0.1:{self.port}{path}?" + urlencode(params)
        with urllib.request.urlopen(url, timeout=2) as resp:
            return json.loads(resp.read().decode())

    def test_wrong_pw_rejected(self):
        self.assertEqual(self._get("/cm", pw="wrong", sid=0, en=1, t=60)["result"],
                         mock_os.R_UNAUTHORIZED)

    def test_correct_pw_accepted(self):
        self.assertEqual(self._get("/cm", pw="deadbeef", sid=0, en=1, t=60)["result"], 1)


class ModelUnitTests(unittest.TestCase):
    """Pure-model tests that don't need the HTTP server."""

    def test_enabled_programs_have_disjoint_station_sets(self):
        # The panel identifies an externally-started program by its live station
        # set, so every program that can actually run (enabled) must own a
        # unique set. Disabled programs never run -> they may reuse stations.
        progs = [p for p in default_programs(len(DEFAULT_NAMES)) if p.enabled]
        sets = [set(p.station_sids()) for p in progs]
        for i in range(len(sets)):
            for j in range(i + 1, len(sets)):
                self.assertEqual(sets[i] & sets[j], set(),
                                 f"enabled programs {i} and {j} share stations")

    def test_durations_length_matches_station_count(self):
        n = len(DEFAULT_NAMES)
        for p in default_programs(n):
            self.assertEqual(len(p.durations), n)


class FixtureCoverageTests(unittest.TestCase):
    """Assert the default fixture exercises the on-device UI edge cases Greg
    needs to eyeball: list pagination, queue overflow, and name wrapping."""

    def setUp(self):
        self.progs = default_programs(len(DEFAULT_NAMES))

    def test_enough_programs_to_paginate(self):
        # Need >2 full pages so the pager dots + ‹ › arrows page 1<->2<->3.
        self.assertGreater(len(self.progs), 2 * PANEL_MAX_PROG_ROWS,
                           "need >2 pages of programs to test the pager")

    def test_a_program_overflows_the_queue_window(self):
        biggest = max(len(p.station_sids()) for p in self.progs)
        self.assertGreater(biggest, PANEL_MAX_QROWS,
                          "need a program with >MAX_QROWS stations to test "
                          "queue windowing + fade indicators")

    def test_long_station_name_belongs_to_an_enabled_program(self):
        # A name long enough to wrap/ellipsize must appear in a program that is
        # enabled, so it actually renders in a running queue.
        long_sids = {i for i, nm in enumerate(DEFAULT_NAMES)
                     if len(nm) > QUEUE_NAME_WRAP_CHARS}
        self.assertTrue(long_sids, "fixture has no long station name")
        covered = set()
        for p in self.progs:
            if p.enabled:
                covered |= set(p.station_sids())
        self.assertTrue(long_sids & covered,
                        "long station name is not in any enabled program")

    def test_a_program_name_overflows_for_ellipsis(self):
        # The programs list must have at least one program whose NAME is long
        # enough to force the name column to ellipsize.
        PROG_NAME_WRAP_CHARS = 24  # ~chars that fit a list-row program name
        self.assertTrue(
            any(len(p.name) > PROG_NAME_WRAP_CHARS for p in self.progs),
            "fixture has no long program name to test list-row ellipsis")


if __name__ == "__main__":
    unittest.main(verbosity=2)
