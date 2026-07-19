// Native (host) unit tests for the hardware-independent panel UI state machine.
// Runs under `pio test -e native` — no board, network, or LVGL required.
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "os_client.h"
#include "panel_state.h"
#include "station_model.h"

using namespace osp;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a transport that always returns a fixed response string.
struct FixedTransport {
  std::string response;
  int calls = 0;
  std::string last_url;

  std::string operator()(const std::string& url) {
    ++calls;
    last_url = url;
    return response;
  }
};

// Build a station model with `n` plain runnable stations.
static StationModel make_model(int n) {
  std::vector<std::string> names;
  for (int i = 0; i < n; ++i)
    names.push_back("S" + std::to_string(i + 1));
  StationModel m;
  m.load(names, {}, 0, 0);
  return m;
}

// Build a minimal JcData indicating nothing is running.
static JcData make_jc_idle(int num_stations = 3, int rssi = -60) {
  JcData jc;
  jc.devt = 1000;
  jc.rssi = rssi;
  // sbits: all off (one byte of zeros per 8 stations)
  jc.sbits.assign((num_stations + 7) / 8, 0);
  // ps: all idle entries
  for (int i = 0; i < num_stations; ++i) {
    PsEntry e;
    e.pid = 0;
    e.rem = 0;
    e.start = 0;
    jc.ps.push_back(e);
  }
  return jc;
}

// Build a JcData with station `sid` running for `rem` seconds.
static JcData make_jc_running(int sid, int rem, int num_stations = 3,
                               int rssi = -60) {
  JcData jc = make_jc_idle(num_stations, rssi);
  // Set the sbits bit for `sid`.
  jc.sbits[sid / 8] |= (1u << (sid % 8));
  jc.ps[sid].pid = 99;
  jc.ps[sid].rem = rem;
  jc.ps[sid].start = 900;
  return jc;
}

// Build a fixture: 3-station model + recording OsClient + PanelState.
struct Fixture {
  StationModel model;
  FixedTransport transport;
  OsClient client;
  PanelState ps;

  Fixture(int n_stations = 3, const std::string& ok = R"({"result":1})")
      : model(make_model(n_stations)),
        client("http://host", "pw",
               [this](const std::string& url) { return transport(url); }),
        ps(model, client) {
    transport.response = ok;
  }
};

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------

void test_initial_state_is_idle() {
  Fixture f;
  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)v.phase);
  TEST_ASSERT_FALSE(v.sleeping);
  TEST_ASSERT_TRUE(v.connected);
  TEST_ASSERT_EQUAL_INT(-1, v.running_sid);
  TEST_ASSERT_EQUAL_INT(0, v.countdown_s);
  TEST_ASSERT_EQUAL_INT(PanelState::kDefaultRunTime, v.run_time_s);
  TEST_ASSERT_FALSE(v.auto_advance);
  TEST_ASSERT_EQUAL_STRING("", v.toast.c_str());
}

// ---------------------------------------------------------------------------
// tick — poll scheduling
// ---------------------------------------------------------------------------

void test_first_tick_triggers_poll() {
  Fixture f;
  // First tick always triggers a poll regardless of interval.
  TEST_ASSERT_TRUE(f.ps.tick(1000));
}

void test_second_tick_does_not_trigger_poll_immediately() {
  Fixture f;
  f.ps.tick(1000);
  TEST_ASSERT_FALSE(f.ps.tick(1001));  // only 1 ms later
}

void test_poll_triggers_after_interval() {
  Fixture f;
  f.ps.tick(0);
  // Just before kPollIntervalMs: no poll.
  TEST_ASSERT_FALSE(f.ps.tick(PanelState::kPollIntervalMs - 1));
  // At kPollIntervalMs: poll due.
  TEST_ASSERT_TRUE(f.ps.tick(PanelState::kPollIntervalMs));
}

// ---------------------------------------------------------------------------
// select_station — idle → running
// ---------------------------------------------------------------------------

void test_select_station_from_idle_starts_running() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);  // tap first station

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(0, v.running_sid);
  TEST_ASSERT_EQUAL_INT(PanelState::kDefaultRunTime, v.countdown_s);
}

void test_select_station_issues_run_station_command() {
  Fixture f;
  f.ps.tick(0);
  f.transport.calls = 0;
  f.ps.select_station(1);

  TEST_ASSERT_EQUAL_INT(1, f.transport.calls);
  TEST_ASSERT_NOT_NULL(strstr(f.transport.last_url.c_str(), "sid=1"));
  TEST_ASSERT_NOT_NULL(strstr(f.transport.last_url.c_str(), "en=1"));
}

void test_select_nonrunnable_station_is_noop() {
  // Build a model with station 1 disabled.
  StationModel model;
  std::vector<uint8_t> stn_dis = {0b00000010};  // disable sid 1
  model.load({"A", "B", "C"}, stn_dis, 0, 0);
  FixedTransport t;
  t.response = R"({"result":1})";
  OsClient client("http://h", "p", [&t](const std::string& u) { return t(u); });
  PanelState ps(model, client);
  ps.tick(0);
  t.calls = 0;

  ps.select_station(1);  // station 1 is disabled

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, t.calls);
}

// ---------------------------------------------------------------------------
// advance / prev
// ---------------------------------------------------------------------------

void test_advance_moves_to_next_station() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);  // start station 0
  f.transport.calls = 0;

  f.ps.advance();

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(1, v.running_sid);
  // advance issues stop(0) then run(1): 2 transport calls.
  TEST_ASSERT_EQUAL_INT(2, f.transport.calls);
}

void test_advance_wraps_from_last_to_first() {
  Fixture f(3);  // stations 0, 1, 2
  f.ps.tick(0);
  f.ps.select_station(2);  // start last station (sid 2)
  f.transport.calls = 0;

  f.ps.advance();  // should wrap to sid 0

  TEST_ASSERT_EQUAL_INT(0, f.ps.view().running_sid);
}

void test_prev_moves_to_previous_station() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(1);
  f.transport.calls = 0;

  f.ps.prev();

  TEST_ASSERT_EQUAL_INT(0, f.ps.view().running_sid);
  TEST_ASSERT_EQUAL_INT(2, f.transport.calls);
}

void test_prev_wraps_from_first_to_last() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.select_station(0);
  f.transport.calls = 0;

  f.ps.prev();  // should wrap to sid 2

  TEST_ASSERT_EQUAL_INT(2, f.ps.view().running_sid);
}

void test_advance_from_idle_is_noop() {
  Fixture f;
  f.ps.tick(0);
  f.transport.calls = 0;

  f.ps.advance();

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.transport.calls);
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------

void test_stop_goes_idle() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);

  f.ps.stop();

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(-1, v.running_sid);
  TEST_ASSERT_EQUAL_STRING("Stopped.", v.toast.c_str());
}

void test_stop_issues_cv_command() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);
  f.transport.calls = 0;

  f.ps.stop();

  TEST_ASSERT_EQUAL_INT(1, f.transport.calls);
  TEST_ASSERT_NOT_NULL(strstr(f.transport.last_url.c_str(), "/cv?"));
  TEST_ASSERT_NOT_NULL(strstr(f.transport.last_url.c_str(), "rsn=1"));
}

void test_stop_from_idle_is_noop() {
  Fixture f;
  f.ps.tick(0);
  f.transport.calls = 0;

  f.ps.stop();

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.transport.calls);
}

// ---------------------------------------------------------------------------
// run_time stepper
// ---------------------------------------------------------------------------

void test_set_run_time_idle_updates_setting() {
  Fixture f;
  f.ps.tick(0);
  f.transport.calls = 0;

  f.ps.set_run_time(120);

  TEST_ASSERT_EQUAL_INT(120, f.ps.view().run_time_s);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.transport.calls);
}

void test_set_run_time_running_extends_station() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);
  f.transport.calls = 0;

  f.ps.set_run_time(180);

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT(180, v.run_time_s);
  TEST_ASSERT_EQUAL_INT(180, v.countdown_s);
  // extend = stop(0) then run(0): 2 calls.
  TEST_ASSERT_EQUAL_INT(2, f.transport.calls);
}

void test_set_run_time_clamped_to_min() {
  Fixture f;
  f.ps.tick(0);
  f.ps.set_run_time(5);  // below minimum
  TEST_ASSERT_EQUAL_INT(PanelState::kMinRunTime, f.ps.view().run_time_s);
}

void test_set_run_time_clamped_to_max() {
  Fixture f;
  f.ps.tick(0);
  f.ps.set_run_time(9999);  // above maximum
  TEST_ASSERT_EQUAL_INT(PanelState::kMaxRunTime, f.ps.view().run_time_s);
}

void test_set_run_time_rounded_to_step() {
  Fixture f;
  f.ps.tick(0);
  f.ps.set_run_time(37);  // not a multiple of 15 → rounds down to 30
  TEST_ASSERT_EQUAL_INT(30, f.ps.view().run_time_s);
}

// ---------------------------------------------------------------------------
// auto_advance toggle
// ---------------------------------------------------------------------------

void test_auto_advance_toggle() {
  Fixture f;
  f.ps.tick(0);
  TEST_ASSERT_FALSE(f.ps.view().auto_advance);

  f.ps.set_auto_advance(true);
  TEST_ASSERT_TRUE(f.ps.view().auto_advance);

  f.ps.set_auto_advance(false);
  TEST_ASSERT_FALSE(f.ps.view().auto_advance);
}

// ---------------------------------------------------------------------------
// countdown via tick
// ---------------------------------------------------------------------------

void test_countdown_decrements_via_tick() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);  // countdown = kDefaultRunTime = 60

  f.ps.tick(1000);  // +1 second
  TEST_ASSERT_EQUAL_INT(59, f.ps.view().countdown_s);

  f.ps.tick(3000);  // +2 more seconds
  TEST_ASSERT_EQUAL_INT(57, f.ps.view().countdown_s);
}

void test_countdown_does_not_decrement_when_idle() {
  Fixture f;
  f.ps.tick(0);
  // Don't start any station; stay idle.
  f.ps.tick(5000);
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().countdown_s);
}

// ---------------------------------------------------------------------------
// station natural expiry (countdown → 0 via tick)
// ---------------------------------------------------------------------------

void test_station_expired_no_auto_advance_goes_idle() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);  // default RT = 60 s

  // Jump time past the run time.
  f.ps.tick(61000);  // +61 seconds

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(-1, v.running_sid);
  // Toast should mention the station number (1-based).
  TEST_ASSERT_NOT_NULL(strstr(v.toast.c_str(), "Station 1"));
}

void test_station_expired_auto_advance_goes_to_next() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.set_auto_advance(true);
  f.ps.select_station(0);  // start station 0, RT=60

  f.transport.calls = 0;
  f.ps.tick(61000);  // expire station 0

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(1, v.running_sid);  // advanced to station 1
  // advance issues stop(0) then run(1).
  TEST_ASSERT_EQUAL_INT(2, f.transport.calls);
}

void test_station_expired_auto_advance_last_goes_idle() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.set_auto_advance(true);
  f.ps.select_station(2);  // last station (sid 2)

  f.ps.tick(61000);  // expire last station

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)v.phase);
  TEST_ASSERT_NOT_NULL(strstr(v.toast.c_str(), "Finished all stations."));
}

void test_auto_advance_chains_correctly() {
  // Auto-advance from station 0 → 1 → 2 → idle.
  Fixture f(3);
  f.ps.tick(0);
  f.ps.set_auto_advance(true);
  f.ps.select_station(0);

  // Station 0 expires.
  f.ps.tick(61000);
  TEST_ASSERT_EQUAL_INT(1, f.ps.view().running_sid);

  // Station 1 expires.
  f.ps.tick(122000);
  TEST_ASSERT_EQUAL_INT(2, f.ps.view().running_sid);

  // Station 2 expires — last one, should stop.
  f.ps.tick(183000);
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
}

// ---------------------------------------------------------------------------
// /jc reconcile
// ---------------------------------------------------------------------------

void test_on_jc_updates_running_sid_and_countdown() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.select_station(0);

  // Poll says station 0 has 45 seconds left.
  f.ps.on_jc(make_jc_running(0, 45));

  TEST_ASSERT_EQUAL_INT(0, f.ps.view().running_sid);
  TEST_ASSERT_EQUAL_INT(45, f.ps.view().countdown_s);
}

void test_on_jc_corrects_to_externally_running_station() {
  // Panel is idle; controller says station 1 is running (external start).
  Fixture f(3);
  f.ps.tick(0);

  f.ps.on_jc(make_jc_running(1, 120));

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(1, v.running_sid);
  TEST_ASSERT_EQUAL_INT(120, v.countdown_s);
}

void test_on_jc_idle_while_running_goes_idle() {
  // Panel thinks it's running; controller shows nothing on.
  Fixture f(3);
  f.ps.tick(0);
  f.ps.select_station(0);

  f.ps.on_jc(make_jc_idle(3));

  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)f.ps.view().phase);
}

void test_on_jc_updates_ctrl_rssi() {
  Fixture f(3);
  f.ps.tick(0);

  f.ps.on_jc(make_jc_idle(3, -72));

  TEST_ASSERT_EQUAL_INT(-72, f.ps.view().ctrl_rssi);
}

void test_on_jc_sets_connected() {
  Fixture f;
  f.ps.tick(0);
  f.ps.on_jc_error();
  TEST_ASSERT_FALSE(f.ps.view().connected);

  f.ps.on_jc(make_jc_idle(3));
  TEST_ASSERT_TRUE(f.ps.view().connected);
}

// ---------------------------------------------------------------------------
// signal loss
// ---------------------------------------------------------------------------

void test_on_jc_error_sets_disconnected() {
  Fixture f;
  f.ps.tick(0);
  f.ps.on_jc_error();
  TEST_ASSERT_FALSE(f.ps.view().connected);
}

void test_signal_loss_does_not_change_running_state() {
  // Station continues running on the controller; panel shows disconnected but
  // does not go idle by itself.
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);

  f.ps.on_jc_error();

  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)f.ps.view().phase);
  TEST_ASSERT_EQUAL_INT(0, f.ps.view().running_sid);
  TEST_ASSERT_FALSE(f.ps.view().connected);
}

void test_signal_recovery_reconnects() {
  Fixture f;
  f.ps.tick(0);
  f.ps.on_jc_error();
  TEST_ASSERT_FALSE(f.ps.view().connected);

  // A successful poll recovers.
  f.ps.on_jc(make_jc_idle(3));
  TEST_ASSERT_TRUE(f.ps.view().connected);
}

// ---------------------------------------------------------------------------
// sleep timer
// ---------------------------------------------------------------------------

void test_sleep_after_idle_timeout() {
  Fixture f;
  f.ps.tick(0);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);

  // Just before timeout: not sleeping.
  f.ps.tick(PanelState::kSleepTimeoutMs - 1);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);

  // At or after timeout: sleeping.
  f.ps.tick(PanelState::kSleepTimeoutMs);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);
}

void test_no_sleep_while_running() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);

  // Advance well past the sleep threshold.
  f.ps.tick(PanelState::kSleepTimeoutMs + 10000);

  TEST_ASSERT_FALSE(f.ps.view().sleeping);
}

void test_touch_wakes_from_sleep() {
  Fixture f;
  f.ps.tick(0);
  // Fast-forward past sleep timeout.
  f.ps.tick(PanelState::kSleepTimeoutMs + 1);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);

  f.ps.on_touch(PanelState::kSleepTimeoutMs + 2);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
}

void test_sleep_timer_resets_on_touch() {
  Fixture f;
  f.ps.tick(0);
  // Touch at T=4min.
  f.ps.on_touch(4 * 60 * 1000);
  // At T=4min+4min59s (9min-1s from start but 4min59s from last touch): no sleep.
  f.ps.tick(4 * 60 * 1000 + PanelState::kSleepTimeoutMs - 1);
  TEST_ASSERT_FALSE(f.ps.view().sleeping);
  // At T=4min+5min (past 5min from last touch): sleep.
  f.ps.tick(4 * 60 * 1000 + PanelState::kSleepTimeoutMs);
  TEST_ASSERT_TRUE(f.ps.view().sleeping);
}

// ---------------------------------------------------------------------------
// jump while running
// ---------------------------------------------------------------------------

void test_jump_to_different_station_while_running() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.select_station(0);  // start station 0
  f.transport.calls = 0;

  f.ps.select_station(2);  // jump to station 2

  const PanelView& v = f.ps.view();
  TEST_ASSERT_EQUAL_INT((int)Phase::Running, (int)v.phase);
  TEST_ASSERT_EQUAL_INT(2, v.running_sid);
  // advance: stop(0) then run(2) = 2 calls.
  TEST_ASSERT_EQUAL_INT(2, f.transport.calls);
}

void test_jump_to_same_station_is_noop() {
  Fixture f(3);
  f.ps.tick(0);
  f.ps.select_station(0);
  f.transport.calls = 0;

  f.ps.select_station(0);  // tap the same station again

  TEST_ASSERT_EQUAL_INT(0, f.transport.calls);
}

// ---------------------------------------------------------------------------
// toast lifecycle
// ---------------------------------------------------------------------------

void test_toast_cleared_after_duration() {
  Fixture f;
  f.ps.tick(0);
  f.ps.select_station(0);

  f.ps.stop();
  TEST_ASSERT_EQUAL_STRING("Stopped.", f.ps.view().toast.c_str());

  // Advance past toast duration.
  f.ps.tick(PanelState::kToastDurationMs + 1);
  TEST_ASSERT_EQUAL_STRING("", f.ps.view().toast.c_str());
}

// ---------------------------------------------------------------------------
// run_time default
// ---------------------------------------------------------------------------

void test_custom_default_run_time() {
  StationModel m = make_model(3);
  FixedTransport t;
  t.response = R"({"result":1})";
  OsClient c("http://h", "p", [&t](const std::string& u) { return t(u); });
  PanelState ps(m, c, 120);
  TEST_ASSERT_EQUAL_INT(120, ps.view().run_time_s);
}

// ---------------------------------------------------------------------------

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_initial_state_is_idle);

  RUN_TEST(test_first_tick_triggers_poll);
  RUN_TEST(test_second_tick_does_not_trigger_poll_immediately);
  RUN_TEST(test_poll_triggers_after_interval);

  RUN_TEST(test_select_station_from_idle_starts_running);
  RUN_TEST(test_select_station_issues_run_station_command);
  RUN_TEST(test_select_nonrunnable_station_is_noop);

  RUN_TEST(test_advance_moves_to_next_station);
  RUN_TEST(test_advance_wraps_from_last_to_first);
  RUN_TEST(test_prev_moves_to_previous_station);
  RUN_TEST(test_prev_wraps_from_first_to_last);
  RUN_TEST(test_advance_from_idle_is_noop);

  RUN_TEST(test_stop_goes_idle);
  RUN_TEST(test_stop_issues_cv_command);
  RUN_TEST(test_stop_from_idle_is_noop);

  RUN_TEST(test_set_run_time_idle_updates_setting);
  RUN_TEST(test_set_run_time_running_extends_station);
  RUN_TEST(test_set_run_time_clamped_to_min);
  RUN_TEST(test_set_run_time_clamped_to_max);
  RUN_TEST(test_set_run_time_rounded_to_step);

  RUN_TEST(test_auto_advance_toggle);

  RUN_TEST(test_countdown_decrements_via_tick);
  RUN_TEST(test_countdown_does_not_decrement_when_idle);

  RUN_TEST(test_station_expired_no_auto_advance_goes_idle);
  RUN_TEST(test_station_expired_auto_advance_goes_to_next);
  RUN_TEST(test_station_expired_auto_advance_last_goes_idle);
  RUN_TEST(test_auto_advance_chains_correctly);

  RUN_TEST(test_on_jc_updates_running_sid_and_countdown);
  RUN_TEST(test_on_jc_corrects_to_externally_running_station);
  RUN_TEST(test_on_jc_idle_while_running_goes_idle);
  RUN_TEST(test_on_jc_updates_ctrl_rssi);
  RUN_TEST(test_on_jc_sets_connected);

  RUN_TEST(test_on_jc_error_sets_disconnected);
  RUN_TEST(test_signal_loss_does_not_change_running_state);
  RUN_TEST(test_signal_recovery_reconnects);

  RUN_TEST(test_sleep_after_idle_timeout);
  RUN_TEST(test_no_sleep_while_running);
  RUN_TEST(test_touch_wakes_from_sleep);
  RUN_TEST(test_sleep_timer_resets_on_touch);

  RUN_TEST(test_jump_to_different_station_while_running);
  RUN_TEST(test_jump_to_same_station_is_noop);

  RUN_TEST(test_toast_cleared_after_duration);

  RUN_TEST(test_custom_default_run_time);

  return UNITY_END();
}
