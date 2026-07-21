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

  return UNITY_END();
}
