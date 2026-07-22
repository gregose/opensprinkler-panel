// Native (host) unit tests for the hardware-independent M6 panel state model.
#include <unity.h>

#include <cstring>
#include <vector>

#include "panel_state.h"
#include "station_model.h"

using namespace osp;

void setUp() {}
void tearDown() {}

static StationModel make_model(int n) {
  std::vector<std::string> names;
  for (int i = 0; i < n; ++i) names.push_back("S" + std::to_string(i + 1));
  StationModel model;
  model.load(names, {}, 0, 0);
  return model;
}

static JcData make_jc_idle(int num_stations = 3, int rssi = -60) {
  JcData jc;
  jc.devt = 1000;
  jc.rssi = rssi;
  jc.sbits.assign((num_stations + 7) / 8, 0);
  jc.ps.resize(num_stations);
  return jc;
}

static JcData make_jc_running(int sid, int rem, int num_stations = 3,
                              int rssi = -60) {
  JcData jc = make_jc_idle(num_stations, rssi);
  jc.sbits[sid / 8] |= static_cast<uint8_t>(1u << (sid % 8));
  jc.ps[sid].pid = 99;
  jc.ps[sid].rem = rem;
  jc.ps[sid].start = 900;
  return jc;
}

// Build a JcData where sid is running as part of a program (pid != 99).
static JcData make_jc_program_running(int sid, int rem, int pid,
                                      int num_stations = 3,
                                      long start_epoch = 800) {
  JcData jc = make_jc_idle(num_stations);
  jc.devt = 1000;
  jc.sbits[sid / 8] |= static_cast<uint8_t>(1u << (sid % 8));
  jc.ps[sid].pid = pid;
  jc.ps[sid].rem = rem;
  jc.ps[sid].start = static_cast<int>(start_epoch);
  return jc;
}

// Build a JpData with nprogs programs (names "P1".."PN").
static JpData make_jp(int nprogs) {
  JpData jp;
  jp.nprogs = nprogs;
  for (int i = 0; i < nprogs; ++i) {
    Program p;
    p.enabled = true;
    p.name = "P" + std::to_string(i + 1);
    p.durations.resize(3, 0);
    jp.programs.push_back(p);
  }
  return jp;
}

struct Fixture {
  StationModel model;
  PanelState ps;

  explicit Fixture(int stations = 3)
      : model(make_model(stations)), ps(model) {
    ps.tick(0);
  }
};

void test_initial_state_is_idle_and_connected() {
  Fixture f;
  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)v.phase);
  TEST_ASSERT_EQUAL_INT((int)LinkState::Connected, (int)v.link);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
  TEST_ASSERT_EQUAL_INT(-1, v.running_sid);
  TEST_ASSERT_EQUAL_INT(PanelState::kDefaultRunTime, v.run_time_s);
}

void test_select_station_queues_single_run_intent_without_optimistic_display() {
  Fixture f;

  f.ps.select_station(1);

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_TRUE(f.ps.pending_sync());
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(1, f.ps.desired().sid);
  TEST_ASSERT_EQUAL_INT(60, f.ps.desired().seconds);
}

void test_latest_intent_wins_while_unconfirmed() {
  Fixture f;

  f.ps.select_station(0);
  f.ps.select_station(2);

  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(2, f.ps.desired().sid);
}

void test_confirmed_poll_clears_run_intent_and_enters_running() {
  Fixture f;

  f.ps.select_station(2);
  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_running(2, 58), 2000);

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(2, v.running_sid);
  TEST_ASSERT_EQUAL_INT(58, v.countdown_s);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

void test_running_station_dead_reckons_between_polls() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 45), 1000);
  f.ps.tick(3000);

  TEST_ASSERT_EQUAL_INT(43, f.ps.view().countdown_s);
}

void test_run_time_change_while_running_updates_value_without_requeue() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 50), 1000);
  f.ps.set_run_time(180);

  TEST_ASSERT_EQUAL_INT(180, f.ps.view().run_time_s);
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(1, f.ps.view().running_sid);
  TEST_ASSERT_EQUAL_INT(50, f.ps.view().countdown_s);
  // Future-runs-only: editing run time must NOT queue a restart of the running station.
  TEST_ASSERT_EQUAL_INT((int)IntentKind::None, (int)f.ps.desired().kind);
}

void test_run_time_change_while_running_used_on_advance() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 50), 1000);
  f.ps.set_run_time(180);
  f.ps.advance();

  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(2, f.ps.desired().sid);
  TEST_ASSERT_EQUAL_INT(180, f.ps.desired().seconds);
}

void test_advance_queues_next_station_without_changing_confirmed_running_sid() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 50), 1000);
  f.ps.advance();

  TEST_ASSERT_EQUAL_INT(1, f.ps.view().running_sid);
  TEST_ASSERT_EQUAL_INT(2, f.ps.desired().sid);
}

void test_prev_wraps_and_queues_previous_station() {
  Fixture f(3);

  f.ps.on_jc(make_jc_running(0, 50), 1000);
  f.ps.prev();

  TEST_ASSERT_EQUAL_INT(2, f.ps.desired().sid);
}

void test_stop_queues_stop_until_idle_confirmed() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 40), 1000);
  f.ps.stop();

  TEST_ASSERT_EQUAL_INT((int)IntentKind::Stop, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);

  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_idle(3), 2000);

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

void test_sync_status_goes_stale_after_timeout_but_keeps_intent() {
  Fixture f;

  f.ps.select_station(0);
  f.ps.on_link_reconnecting(1000);
  f.ps.tick(PanelState::kSyncTimeoutMs + 1001);

  TEST_ASSERT_TRUE(f.ps.pending_sync());
  TEST_ASSERT_TRUE(f.ps.sync_stale());
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
}

void test_link_state_distinguishes_reconnecting_offline_and_auth() {
  Fixture f;

  f.ps.on_link_reconnecting(1000);
  TEST_ASSERT_EQUAL_INT((int)LinkState::Reconnecting, (int)f.ps.view().link);

  f.ps.on_link_offline(2000);
  TEST_ASSERT_EQUAL_INT((int)LinkState::Offline, (int)f.ps.view().link);

  f.ps.on_auth_error(3000);
  TEST_ASSERT_EQUAL_INT((int)LinkState::AuthError, (int)f.ps.view().link);

  f.ps.on_link_connected(4000);
  TEST_ASSERT_EQUAL_INT((int)LinkState::Connected, (int)f.ps.view().link);
}

void test_offline_countdown_to_zero_holds_running_at_zero_until_jc_confirms_idle() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 1), 1000);
  f.ps.on_link_offline(1500);
  f.ps.tick(2000);

  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(1, f.ps.view().running_sid);
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().countdown_s);
  TEST_ASSERT_TRUE(f.ps.awaiting_close());
  TEST_ASSERT_TRUE(f.ps.pending_sync());

  f.ps.on_jc(make_jc_idle(3), 4000);

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.awaiting_close());
}

void test_auto_advance_queues_next_station_after_close_confirmation() {
  Fixture f(3);

  f.ps.set_auto_advance(true);
  f.ps.on_jc(make_jc_running(0, 1), 1000);
  f.ps.tick(2000);

  TEST_ASSERT_TRUE(f.ps.awaiting_close());
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(1, f.ps.desired().sid);
  TEST_ASSERT_FALSE(f.ps.can_deliver_desired());

  f.ps.on_jc(make_jc_idle(3), 4000);

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.awaiting_close());
  TEST_ASSERT_TRUE(f.ps.can_deliver_desired());
  TEST_ASSERT_EQUAL_INT(1, f.ps.desired().sid);
}

void test_last_auto_advance_station_finishes_idle() {
  Fixture f(2);

  f.ps.set_auto_advance(true);
  f.ps.on_jc(make_jc_running(1, 1), 1000);
  f.ps.tick(2000);
  TEST_ASSERT_TRUE(f.ps.awaiting_close());
  TEST_ASSERT_FALSE(f.ps.has_desired());

  f.ps.on_jc(make_jc_idle(2), 3000);

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.awaiting_close());
  TEST_ASSERT_FALSE(f.ps.has_desired());
}

void test_station_list_loaded_flag_tracks_jn_readiness() {
  Fixture f;

  TEST_ASSERT_FALSE(f.ps.view().station_list_loaded);
  f.ps.set_station_list_loaded(true);
  TEST_ASSERT_TRUE(f.ps.view().station_list_loaded);
}

void test_auth_error_does_not_clear_pending_intent() {
  Fixture f;

  f.ps.select_station(0);
  f.ps.on_auth_error(1000);

  TEST_ASSERT_EQUAL_INT((int)LinkState::AuthError, (int)f.ps.view().link);
  TEST_ASSERT_TRUE(f.ps.pending_sync());
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Run, (int)f.ps.desired().kind);
}

void test_sleep_wakes_on_touch() {
  Fixture f;

  f.ps.tick(PanelState::kDefaultSleepTimeoutMs + 1);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);

  f.ps.on_touch(PanelState::kDefaultSleepTimeoutMs + 2);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
}

void test_sleep_timeout_configurable() {
  Fixture f;

  // Default is the production timeout.
  TEST_ASSERT_EQUAL_UINT32(PanelState::kDefaultSleepTimeoutMs,
                           f.ps.sleep_timeout_ms());

  // Shorten it (as the NVS/config path would): sleeps at the new threshold.
  f.ps.set_sleep_timeout_ms(30000);
  TEST_ASSERT_EQUAL_UINT32(30000, f.ps.sleep_timeout_ms());

  f.ps.tick(29999);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
  f.ps.tick(30000);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);
}

void test_sleep_timeout_zero_disables() {
  Fixture f;

  f.ps.set_sleep_timeout_ms(0);
  // Never sleeps regardless of how much idle time elapses.
  f.ps.tick(PanelState::kDefaultSleepTimeoutMs * 10);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
}

void test_idle_elapsed_ms_tracks_since_touch() {
  Fixture f;

  f.ps.tick(1000);       // last_touch primed at 0 by the fixture
  f.ps.on_touch(5000);
  f.ps.tick(8000);
  TEST_ASSERT_EQUAL_UINT32(3000, f.ps.idle_elapsed_ms());
}

// Regression for #60: on_jc() re-affirms idle via enter_idle() on every ~2 s
// poll. That must NOT reset the idle-sleep clock, or idle_elapsed_ms() is
// capped at the poll interval and the screen never sleeps.
void test_idle_timer_survives_repeated_jc_polls() {
  Fixture f;  // ticked at 0 -> idle clock primed at 0
  f.ps.set_sleep_timeout_ms(30000);

  uint32_t t = 0;
  for (int i = 0; i < 20; ++i) {
    t += 2000;                       // simulate the ~2 s /jc poll cadence
    f.ps.on_jc(make_jc_idle(), t);   // re-affirms idle every poll
    f.ps.tick(t);
    // Idle age must track wall time, not reset on each poll.
    TEST_ASSERT_EQUAL_UINT32(t, f.ps.idle_elapsed_ms());
    if (t < 30000) TEST_ASSERT_FALSE(f.ps.view().sleeping);
  }
  TEST_ASSERT_TRUE(f.ps.view().sleeping);
}

// A genuine Running->Idle transition DOES reset the idle clock (so a fresh
// timeout starts when a run ends), unlike a re-affirming idle poll.
void test_running_to_idle_transition_resets_idle_clock() {
  Fixture f;

  f.ps.on_jc(make_jc_running(1, 60), 10000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);

  // Station stops -> transition back to idle at t=50000.
  f.ps.on_jc(make_jc_idle(), 50000);
  f.ps.tick(50000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  // Idle clock reset at the transition, not counting the running period.
  TEST_ASSERT_EQUAL_UINT32(0, f.ps.idle_elapsed_ms());
}

// ---------------------------------------------------------------------------
// M9 — program screen classification
// ---------------------------------------------------------------------------

void test_manual_run_classified_as_running_phase() {
  Fixture f;
  // pid=99 means manual run
  f.ps.on_jc(make_jc_running(0, 45), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().running_sid);
}

void test_program_run_classified_as_program_running_phase() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.on_jc(make_jc_program_running(0, 45, 2), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().prog_run.current_sid);
  TEST_ASSERT_EQUAL_INT((int)RunClass::ProgramRun,
                        (int)f.ps.view().prog_run.run_class);
}

void test_idle_jc_stays_idle_phase() {
  Fixture f;
  f.ps.on_jc(make_jc_idle(), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
}

void test_program_run_then_idle_returns_to_idle_phase() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.on_jc(make_jc_program_running(0, 45, 1), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);

  f.ps.on_jc(make_jc_idle(), 3000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
}

void test_jc_fields_stored_in_view() {
  Fixture f;
  JcData jc = make_jc_idle();
  jc.devt = 5000;
  jc.sunrise = 370;
  jc.sunset = 1140;
  jc.pq = 1;
  jc.pt = 240;
  f.ps.on_jc(jc, 2000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(5000, (int)f.ps.view().ctrl_devt);
  TEST_ASSERT_EQUAL_INT(370, f.ps.view().sunrise_min);
  TEST_ASSERT_EQUAL_INT(1140, f.ps.view().sunset_min);
  TEST_ASSERT_TRUE(f.ps.view().paused);
  TEST_ASSERT_EQUAL_INT(240, f.ps.view().pause_remaining_s);
}

// ---------------------------------------------------------------------------
// M9 — programs list navigation
// ---------------------------------------------------------------------------

void test_open_programs_list_only_from_idle() {
  Fixture f;
  f.ps.tick(0);

  // From Idle: should work.
  f.ps.open_programs_list();
  TEST_ASSERT_TRUE(f.ps.view().showing_programs_list);

  // Close and re-check.
  f.ps.close_programs_list();
  TEST_ASSERT_FALSE(f.ps.view().showing_programs_list);
}

void test_open_programs_list_not_allowed_while_running() {
  Fixture f;
  f.ps.on_jc(make_jc_running(0, 45), 1000);
  f.ps.open_programs_list();
  TEST_ASSERT_FALSE(f.ps.view().showing_programs_list);
}

void test_programs_list_page_clamps_to_valid_range() {
  Fixture f;
  f.ps.set_program_list(make_jp(9));  // 9 programs -> 3 pages (0,1,2)
  f.ps.open_programs_list();

  f.ps.set_prog_list_page(1);
  TEST_ASSERT_EQUAL_INT(1, f.ps.view().prog_list_page);

  f.ps.set_prog_list_page(5);   // beyond max page
  TEST_ASSERT_EQUAL_INT(2, f.ps.view().prog_list_page);

  f.ps.set_prog_list_page(-1);  // below 0
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().prog_list_page);
}

void test_programs_list_closed_when_program_run_starts() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.open_programs_list();
  TEST_ASSERT_TRUE(f.ps.view().showing_programs_list);

  f.ps.on_jc(make_jc_program_running(0, 45, 1), 1000);
  TEST_ASSERT_FALSE(f.ps.view().showing_programs_list);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
}

// ---------------------------------------------------------------------------
// M9 — run_initiated_by_panel / backlight behavior
// ---------------------------------------------------------------------------

void test_external_program_run_allows_sleep() {
  Fixture f;
  f.ps.set_sleep_timeout_ms(5000);
  f.ps.set_program_list(make_jp(2));

  // Controller starts a program without panel interaction.
  f.ps.on_jc(make_jc_program_running(0, 300, 1), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.view().run_initiated_by_panel);

  // After sleep timeout elapses, panel should sleep.
  f.ps.tick(6001);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);
}

void test_panel_initiated_program_run_stays_awake() {
  Fixture f;
  f.ps.set_sleep_timeout_ms(5000);
  f.ps.set_program_list(make_jp(2));

  // Panel initiates a program run.
  f.ps.run_program_intent(1);
  TEST_ASSERT_TRUE(f.ps.view().run_initiated_by_panel);
  f.ps.mark_desired_delivered();

  // Controller confirms the run.
  f.ps.on_jc(make_jc_program_running(0, 300, 1), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
  TEST_ASSERT_TRUE(f.ps.view().run_initiated_by_panel);

  // Sleep timeout passes — but panel-initiated run should NOT sleep.
  f.ps.tick(6001);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
}

void test_run_initiated_flag_cleared_on_idle() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.run_program_intent(1);
  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_program_running(0, 45, 1), 1000);
  TEST_ASSERT_TRUE(f.ps.view().run_initiated_by_panel);

  f.ps.on_jc(make_jc_idle(), 3000);
  TEST_ASSERT_FALSE(f.ps.view().run_initiated_by_panel);
}

void test_manual_station_run_sets_run_initiated_by_panel() {
  Fixture f;
  f.ps.select_station(0);
  TEST_ASSERT_TRUE(f.ps.view().run_initiated_by_panel);
}

// ---------------------------------------------------------------------------
// M9 — program action intents
// ---------------------------------------------------------------------------

void test_run_program_intent_queues_run_program_kind() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.run_program_intent(2);

  TEST_ASSERT_EQUAL_INT((int)IntentKind::RunProgram, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(2, f.ps.desired().sid);
  TEST_ASSERT_TRUE(f.ps.pending_sync());
}

void test_run_program_intent_confirmed_when_program_running() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.run_program_intent(1);
  f.ps.mark_desired_delivered();

  f.ps.on_jc(make_jc_program_running(0, 60, 1), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

void test_toggle_program_enabled_intent_queues_correctly() {
  Fixture f;
  f.ps.toggle_program_enabled_intent(3, true);
  TEST_ASSERT_EQUAL_INT((int)IntentKind::SetProgramEnabled, (int)f.ps.desired().kind);
  TEST_ASSERT_EQUAL_INT(3, f.ps.desired().sid);
  TEST_ASSERT_EQUAL_INT(1, f.ps.desired().seconds);

  f.ps.toggle_program_enabled_intent(3, false);
  TEST_ASSERT_EQUAL_INT(0, f.ps.desired().seconds);
}

void test_set_program_enabled_confirmed_after_delivery() {
  Fixture f;
  f.ps.toggle_program_enabled_intent(2, false);
  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_idle(), 1000);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

void test_pause_toggle_intent_queues_correctly() {
  Fixture f;
  f.ps.pause_toggle_intent();
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Pause, (int)f.ps.desired().kind);
  TEST_ASSERT_TRUE(f.ps.pending_sync());
}

void test_pause_intent_confirmed_after_delivery() {
  Fixture f;
  f.ps.pause_toggle_intent();
  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_idle(), 1000);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

// ---------------------------------------------------------------------------
// M9 — stop from ProgramRunning phase
// ---------------------------------------------------------------------------

void test_stop_from_program_running_queues_stop_intent() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.on_jc(make_jc_program_running(0, 60, 1), 1000);
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);

  f.ps.stop();
  TEST_ASSERT_EQUAL_INT((int)IntentKind::Stop, (int)f.ps.desired().kind);

  f.ps.mark_desired_delivered();
  f.ps.on_jc(make_jc_idle(), 2000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_FALSE(f.ps.pending_sync());
}

// ---------------------------------------------------------------------------
// M9 — countdown dead-reckoning for ProgramRunning
// ---------------------------------------------------------------------------

void test_program_running_countdown_dead_reckons() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.on_jc(make_jc_program_running(0, 45, 1), 1000);
  TEST_ASSERT_EQUAL_INT(45, f.ps.view().countdown_s);

  f.ps.tick(3000);
  TEST_ASSERT_EQUAL_INT(43, f.ps.view().countdown_s);
}

void test_program_running_countdown_does_not_trigger_await_close() {
  Fixture f;
  f.ps.set_program_list(make_jp(2));
  f.ps.on_jc(make_jc_program_running(0, 1, 1), 1000);
  f.ps.tick(2000);

  // Countdown reaches 0 — but ProgramRunning should NOT enter await_close.
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().countdown_s);
  TEST_ASSERT_FALSE(f.ps.awaiting_close());
  // Phase stays ProgramRunning until JC confirms idle.
  TEST_ASSERT_EQUAL_INT((int)Phase::ProgramRunning, (int)f.ps.view().phase);
}

// ---------------------------------------------------------------------------
// M9 — set_program_list
// ---------------------------------------------------------------------------

void test_set_program_list_updates_cache() {
  Fixture f;
  TEST_ASSERT_EQUAL_INT(0, (int)f.ps.program_list().programs.size());

  f.ps.set_program_list(make_jp(4));
  TEST_ASSERT_EQUAL_INT(4, (int)f.ps.program_list().programs.size());
  TEST_ASSERT_EQUAL_INT(4, f.ps.program_list().nprogs);
}



int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_initial_state_is_idle_and_connected);
  RUN_TEST(test_select_station_queues_single_run_intent_without_optimistic_display);
  RUN_TEST(test_latest_intent_wins_while_unconfirmed);
  RUN_TEST(test_confirmed_poll_clears_run_intent_and_enters_running);
  RUN_TEST(test_running_station_dead_reckons_between_polls);
  RUN_TEST(test_run_time_change_while_running_updates_value_without_requeue);
  RUN_TEST(test_run_time_change_while_running_used_on_advance);
  RUN_TEST(test_advance_queues_next_station_without_changing_confirmed_running_sid);
  RUN_TEST(test_prev_wraps_and_queues_previous_station);
  RUN_TEST(test_stop_queues_stop_until_idle_confirmed);
  RUN_TEST(test_sync_status_goes_stale_after_timeout_but_keeps_intent);
  RUN_TEST(test_link_state_distinguishes_reconnecting_offline_and_auth);
  RUN_TEST(test_offline_countdown_to_zero_holds_running_at_zero_until_jc_confirms_idle);
  RUN_TEST(test_auto_advance_queues_next_station_after_close_confirmation);
  RUN_TEST(test_last_auto_advance_station_finishes_idle);
  RUN_TEST(test_station_list_loaded_flag_tracks_jn_readiness);
  RUN_TEST(test_auth_error_does_not_clear_pending_intent);
  RUN_TEST(test_sleep_wakes_on_touch);
  RUN_TEST(test_sleep_timeout_configurable);
  RUN_TEST(test_sleep_timeout_zero_disables);
  RUN_TEST(test_idle_elapsed_ms_tracks_since_touch);
  RUN_TEST(test_idle_timer_survives_repeated_jc_polls);
  RUN_TEST(test_running_to_idle_transition_resets_idle_clock);

  // M9 — program screen classification
  RUN_TEST(test_manual_run_classified_as_running_phase);
  RUN_TEST(test_program_run_classified_as_program_running_phase);
  RUN_TEST(test_idle_jc_stays_idle_phase);
  RUN_TEST(test_program_run_then_idle_returns_to_idle_phase);
  RUN_TEST(test_jc_fields_stored_in_view);

  // M9 — programs list navigation
  RUN_TEST(test_open_programs_list_only_from_idle);
  RUN_TEST(test_open_programs_list_not_allowed_while_running);
  RUN_TEST(test_programs_list_page_clamps_to_valid_range);
  RUN_TEST(test_programs_list_closed_when_program_run_starts);

  // M9 — run_initiated_by_panel / backlight
  RUN_TEST(test_external_program_run_allows_sleep);
  RUN_TEST(test_panel_initiated_program_run_stays_awake);
  RUN_TEST(test_run_initiated_flag_cleared_on_idle);
  RUN_TEST(test_manual_station_run_sets_run_initiated_by_panel);

  // M9 — program action intents
  RUN_TEST(test_run_program_intent_queues_run_program_kind);
  RUN_TEST(test_run_program_intent_confirmed_when_program_running);
  RUN_TEST(test_toggle_program_enabled_intent_queues_correctly);
  RUN_TEST(test_set_program_enabled_confirmed_after_delivery);
  RUN_TEST(test_pause_toggle_intent_queues_correctly);
  RUN_TEST(test_pause_intent_confirmed_after_delivery);

  // M9 — stop from ProgramRunning
  RUN_TEST(test_stop_from_program_running_queues_stop_intent);

  // M9 — countdown dead-reckoning for ProgramRunning
  RUN_TEST(test_program_running_countdown_dead_reckons);
  RUN_TEST(test_program_running_countdown_does_not_trigger_await_close);

  // M9 — set_program_list
  RUN_TEST(test_set_program_list_updates_cache);

  return UNITY_END();
}
