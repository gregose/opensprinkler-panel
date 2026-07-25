#!/usr/bin/env python3
"""
Connection + API-contract tests for the mock OpenSprinkler controller.

These are the "connection tests" that let you trust the mock as a manual-testing
stand-in: they boot the mock on an ephemeral local port and exercise every
endpoint the panel firmware uses, asserting the exact JSON shape and the tricky
behaviours (the en=1 no-op quirk, sequential program queue, pid 254 vs real pid,
pause, enable/disable).

Run:
    python3 docs/test_mock_os.py            # verbose unittest run
    python3 -m unittest docs.test_mock_os   # via unittest discovery

Pure standard library — no pytest / third-party deps.
"""

import json
import threading
import unittest
import urllib.request
from urllib.parse import urlencode

import mock_os
from mock_os import (
    FLAG_ENABLED, MockController, build_server, default_programs,
)


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


class CmStationTests(MockServerCase):
    def _running_sid(self, jc):
        for sid, e in enumerate(jc["ps"]):
            if e[0] != 0 and e[1] > 0:
                return sid
        return None

    def test_run_station_turns_it_on(self):
        self.assertEqual(self.get("/cm", sid=0, en=1, t=120)["result"], 1)
        jc = self.get("/jc")
        self.assertEqual(jc["ps"][0][0], 99)      # manual pid
        self.assertGreater(jc["ps"][0][1], 0)     # rem > 0
        self.assertEqual(self._running_sid(jc), 0)

    def test_en1_on_running_station_is_noop(self):
        self.get("/cm", sid=0, en=1, t=120)
        rem1 = self.get("/jc")["ps"][0][1]
        # Re-issue en=1 with a *different* duration; the quirk means no change.
        self.assertEqual(self.get("/cm", sid=0, en=1, t=600)["result"], 1)
        rem2 = self.get("/jc")["ps"][0][1]
        self.assertLessEqual(rem2, rem1)          # counting down, not reset to 600
        self.assertLess(rem2, 300)                # definitely not the new 600

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

    def test_default_programs_have_disjoint_station_sets(self):
        progs = default_programs(14)
        sets = [set(p.station_sids()) for p in progs]
        for i in range(len(sets)):
            for j in range(i + 1, len(sets)):
                self.assertEqual(sets[i] & sets[j], set(),
                                 f"programs {i} and {j} share stations")

    def test_durations_length_matches_station_count(self):
        for p in default_programs(14):
            self.assertEqual(len(p.durations), 14)


if __name__ == "__main__":
    unittest.main(verbosity=2)
