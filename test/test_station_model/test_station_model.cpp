// Native (host) unit tests for the hardware-independent station domain model.
// Runs under `pio test -e native` — no board or network required.
#include <unity.h>

#include <string>
#include <vector>

#include "station_model.h"

using namespace osp;

void setUp() {}
void tearDown() {}

// --- board_bit_set ----------------------------------------------------------

void test_board_bit_set_basic() {
  // Board 0: stations 0 and 3 set; board 1: station 8 (bit 0) set.
  std::vector<uint8_t> boards = {0b00001001, 0b00000001};
  TEST_ASSERT_TRUE(board_bit_set(boards, 0));
  TEST_ASSERT_FALSE(board_bit_set(boards, 1));
  TEST_ASSERT_TRUE(board_bit_set(boards, 3));
  TEST_ASSERT_TRUE(board_bit_set(boards, 8));
  TEST_ASSERT_FALSE(board_bit_set(boards, 9));
}

void test_board_bit_set_out_of_range() {
  std::vector<uint8_t> boards = {0xFF};
  TEST_ASSERT_FALSE(board_bit_set(boards, -1));
  TEST_ASSERT_FALSE(board_bit_set(boards, 8));   // second byte doesn't exist
  TEST_ASSERT_FALSE(board_bit_set({}, 0));
}

// --- grid_layout ------------------------------------------------------------

void test_grid_layout_single_row() {
  TEST_ASSERT_EQUAL_INT(0, grid_layout(0).rows);
  for (int n = 1; n <= 12; ++n) {
    GridLayout g = grid_layout(n);
    TEST_ASSERT_EQUAL_INT(1, g.rows);
    TEST_ASSERT_EQUAL_INT(n, g.cols);
  }
}

void test_grid_layout_two_rows() {
  // 13..24 -> 2 balanced rows.
  GridLayout g13 = grid_layout(13);
  TEST_ASSERT_EQUAL_INT(2, g13.rows);
  TEST_ASSERT_EQUAL_INT(7, g13.cols);  // ceil(13/2)

  GridLayout g14 = grid_layout(14);
  TEST_ASSERT_EQUAL_INT(2, g14.rows);
  TEST_ASSERT_EQUAL_INT(7, g14.cols);  // 2x7

  GridLayout g24 = grid_layout(24);
  TEST_ASSERT_EQUAL_INT(2, g24.rows);
  TEST_ASSERT_EQUAL_INT(12, g24.cols);  // 2x12
}

// --- rssi_to_bars -----------------------------------------------------------

void test_rssi_to_bars() {
  TEST_ASSERT_EQUAL_INT(4, rssi_to_bars(-30));
  TEST_ASSERT_EQUAL_INT(4, rssi_to_bars(-55));
  TEST_ASSERT_EQUAL_INT(3, rssi_to_bars(-56));
  TEST_ASSERT_EQUAL_INT(3, rssi_to_bars(-65));
  TEST_ASSERT_EQUAL_INT(2, rssi_to_bars(-66));
  TEST_ASSERT_EQUAL_INT(2, rssi_to_bars(-72));
  TEST_ASSERT_EQUAL_INT(1, rssi_to_bars(-73));
  TEST_ASSERT_EQUAL_INT(1, rssi_to_bars(-82));
  TEST_ASSERT_EQUAL_INT(0, rssi_to_bars(-83));
  TEST_ASSERT_EQUAL_INT(0, rssi_to_bars(-100));
}

// --- display_bars -----------------------------------------------------------

void test_display_bars() {
  // connected=false always returns 0 regardless of quality
  TEST_ASSERT_EQUAL_INT(0, display_bars(0, false));
  TEST_ASSERT_EQUAL_INT(0, display_bars(4, false));
  TEST_ASSERT_EQUAL_INT(0, display_bars(2, false));
  // connected=true floors at 1 (quality 0 -> 1)
  TEST_ASSERT_EQUAL_INT(1, display_bars(0, true));
  // connected=true passes quality through for 1..4
  TEST_ASSERT_EQUAL_INT(1, display_bars(1, true));
  TEST_ASSERT_EQUAL_INT(2, display_bars(2, true));
  TEST_ASSERT_EQUAL_INT(4, display_bars(4, true));
}

// --- StationModel: filtering ------------------------------------------------

static std::vector<std::string> names14() {
  return {"Front Lawn", "Driveway",  "North Beds", "Back Lawn", "Back Beds",
          "Patio Pots", "Side Yard", "Veg Garden", "Rear Rotors", "Parkway",
          "Front Rotors", "Mailbox", "Pool Deck", "Fence Line"};
}

void test_model_all_runnable() {
  StationModel m;
  m.load(names14(), {}, 0, 0);
  TEST_ASSERT_EQUAL_INT(14, m.runnable_count());
  TEST_ASSERT_EQUAL_INT(0, m.runnable_sids().front());
  TEST_ASSERT_EQUAL_INT(13, m.runnable_sids().back());
}

void test_model_filters_disabled_and_master() {
  StationModel m;
  // Disable station 2; master1 = station 6 (mas=6 -> sid 5), master2 = station
  // 10 (mas2=10 -> sid 9). mas/mas2 are 1-based; 0 = none.
  std::vector<uint8_t> stn_dis = {0b00000100, 0};  // sid 2
  m.load(names14(), stn_dis, /*mas=*/6, /*mas2=*/10);

  TEST_ASSERT_EQUAL_INT(11, m.runnable_count());  // 14 - 3
  TEST_ASSERT_EQUAL_INT(-1, m.runnable_index(2));
  TEST_ASSERT_EQUAL_INT(-1, m.runnable_index(5));
  TEST_ASSERT_EQUAL_INT(-1, m.runnable_index(9));
  TEST_ASSERT_TRUE(m.stations()[2].disabled);
  TEST_ASSERT_TRUE(m.stations()[5].master);
  TEST_ASSERT_TRUE(m.stations()[9].master);
}

// Regression tests for the empty-grid bug (#39): masop/masop2 association masks
// must NOT be treated as masters. Uses Greg's real controller config: 24
// stations, stn_dis=[0,192,255] (sids 14-23 disabled), masop all-ones (every
// zone opens the pump), mas=mas2=0 (no master) -> 14 runnable (sids 0-13).
static std::vector<std::string> names24() {
  std::vector<std::string> n = names14();
  for (int i = 15; i <= 24; ++i) n.push_back("S" + std::to_string(i));
  return n;  // 24 names total
}

void test_model_pump_association_not_master() {
  StationModel m;
  std::vector<uint8_t> stn_dis = {0x00, 0xC0, 0xFF};  // sids 14-23 disabled
  m.load(names24(), stn_dis, /*mas=*/0, /*mas2=*/0);
  TEST_ASSERT_EQUAL_INT(14, m.runnable_count());
  TEST_ASSERT_EQUAL_INT(0, m.runnable_sids().front());
  TEST_ASSERT_EQUAL_INT(13, m.runnable_sids().back());
  for (int sid = 0; sid <= 13; ++sid)
    TEST_ASSERT_FALSE(m.stations()[sid].master);
}

void test_model_master_index_filtered() {
  StationModel m;
  std::vector<uint8_t> stn_dis = {0x00, 0xC0, 0xFF};  // sids 14-23 disabled
  // mas=1 -> station 1 (sid 0) is the master; must drop to 13 runnable.
  m.load(names24(), stn_dis, /*mas=*/1, /*mas2=*/0);
  TEST_ASSERT_EQUAL_INT(13, m.runnable_count());
  TEST_ASSERT_TRUE(m.stations()[0].master);
  TEST_ASSERT_EQUAL_INT(-1, m.runnable_index(0));
  TEST_ASSERT_EQUAL_INT(1, m.runnable_sids().front());
}

// --- StationModel: navigation ----------------------------------------------

void test_navigation_wraps() {
  StationModel m;
  m.load(names14(), {}, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, m.next_sid(0));
  TEST_ASSERT_EQUAL_INT(0, m.next_sid(13));   // wrap forward
  TEST_ASSERT_EQUAL_INT(13, m.prev_sid(0));   // wrap backward
  TEST_ASSERT_EQUAL_INT(12, m.prev_sid(13));
}

void test_navigation_skips_nonrunnable() {
  StationModel m;
  std::vector<uint8_t> stn_dis = {0b00000010, 0};  // disable sid 1
  m.load(names14(), stn_dis, 0, 0);
  TEST_ASSERT_EQUAL_INT(2, m.next_sid(0));   // skips disabled sid 1
  TEST_ASSERT_EQUAL_INT(0, m.prev_sid(2));   // skips it going back
}

void test_navigation_from_unknown_sid() {
  StationModel m;
  m.load(names14(), {}, 0, 0);
  TEST_ASSERT_EQUAL_INT(0, m.next_sid(999));   // -> first runnable
  TEST_ASSERT_EQUAL_INT(13, m.prev_sid(999));  // -> last runnable
}

void test_navigation_empty() {
  StationModel m;
  m.load({}, {}, 0, 0);
  TEST_ASSERT_EQUAL_INT(-1, m.next_sid(0));
  TEST_ASSERT_EQUAL_INT(-1, m.prev_sid(0));
  TEST_ASSERT_EQUAL_INT(0, m.layout().rows);
}

// --- StationModel: auto-advance (stops after last) --------------------------

void test_auto_advance_stops_after_last() {
  StationModel m;
  m.load(names14(), {}, 0, 0);
  TEST_ASSERT_EQUAL_INT(1, m.auto_next_sid(0));
  TEST_ASSERT_EQUAL_INT(13, m.auto_next_sid(12));
  TEST_ASSERT_EQUAL_INT(-1, m.auto_next_sid(13));  // last -> stop, no wrap
}

void test_auto_advance_last_is_last_runnable() {
  StationModel m;
  // Disable the final two stations; last runnable is sid 11.
  std::vector<uint8_t> stn_dis = {0, 0b00110000};  // sids 12, 13
  m.load(names14(), stn_dis, 0, 0);
  TEST_ASSERT_EQUAL_INT(11, m.runnable_sids().back());
  TEST_ASSERT_EQUAL_INT(-1, m.auto_next_sid(11));  // stops after real last
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_board_bit_set_basic);
  RUN_TEST(test_board_bit_set_out_of_range);
  RUN_TEST(test_grid_layout_single_row);
  RUN_TEST(test_grid_layout_two_rows);
  RUN_TEST(test_rssi_to_bars);
  RUN_TEST(test_display_bars);
  RUN_TEST(test_model_all_runnable);
  RUN_TEST(test_model_filters_disabled_and_master);
  RUN_TEST(test_model_pump_association_not_master);
  RUN_TEST(test_model_master_index_filtered);
  RUN_TEST(test_navigation_wraps);
  RUN_TEST(test_navigation_skips_nonrunnable);
  RUN_TEST(test_navigation_from_unknown_sid);
  RUN_TEST(test_navigation_empty);
  RUN_TEST(test_auto_advance_stops_after_last);
  RUN_TEST(test_auto_advance_last_is_last_runnable);
  return UNITY_END();
}
