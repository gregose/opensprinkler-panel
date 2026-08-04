// Native (host) unit tests for the hardware-independent battery monitor.
// Runs under `pio test -e native` — no board or ADC required.
#include <unity.h>

#include "battery_monitor.h"

using namespace osp;

void setUp() {}
void tearDown() {}

// --- battery_vbat_from_tap_mv ----------------------------------------------

void test_vbat_from_tap_mv() {
  // Confirmed on-device: tap 1728 mV -> ~3456 mV VBAT (ratio 2.0).
  TEST_ASSERT_EQUAL_INT(3456, battery_vbat_from_tap_mv(1728));
  TEST_ASSERT_EQUAL_INT(4200, battery_vbat_from_tap_mv(2100));
  TEST_ASSERT_EQUAL_INT(0, battery_vbat_from_tap_mv(0));
  TEST_ASSERT_EQUAL_INT(0, battery_vbat_from_tap_mv(-50));  // clamps negatives
}

// --- battery_percent_from_mv -----------------------------------------------

void test_percent_endpoints_and_clamps() {
  TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4200));
  TEST_ASSERT_EQUAL_INT(100, battery_percent_from_mv(4500));  // above full clamps
  TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(3000));
  TEST_ASSERT_EQUAL_INT(0, battery_percent_from_mv(2500));    // below empty clamps
}

void test_percent_curve_knots() {
  // Exact curve knots.
  TEST_ASSERT_EQUAL_INT(90, battery_percent_from_mv(4100));
  TEST_ASSERT_EQUAL_INT(80, battery_percent_from_mv(4000));
  TEST_ASSERT_EQUAL_INT(65, battery_percent_from_mv(3900));
  TEST_ASSERT_EQUAL_INT(50, battery_percent_from_mv(3800));
  TEST_ASSERT_EQUAL_INT(35, battery_percent_from_mv(3700));
  TEST_ASSERT_EQUAL_INT(20, battery_percent_from_mv(3600));
}

void test_percent_interpolates_between_knots() {
  // Midpoint of 3800(50) and 3900(65) -> ~57-58.
  const int mid = battery_percent_from_mv(3850);
  TEST_ASSERT_INT_WITHIN(1, 57, mid);
  // The bench's ~3.46 V real cell reads low but non-zero.
  const int low = battery_percent_from_mv(3456);
  TEST_ASSERT_TRUE(low > 0 && low < 12);
}

void test_percent_is_monotonic_non_decreasing() {
  int prev = -1;
  for (int mv = 2800; mv <= 4400; mv += 10) {
    const int p = battery_percent_from_mv(mv);
    TEST_ASSERT_TRUE(p >= prev);
    TEST_ASSERT_TRUE(p >= 0 && p <= 100);
    prev = p;
  }
}

// --- battery_tier_from_percent ---------------------------------------------

void test_tier_thresholds() {
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Healthy, (int)battery_tier_from_percent(100));
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Healthy, (int)battery_tier_from_percent(51));
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Low, (int)battery_tier_from_percent(50));
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Low, (int)battery_tier_from_percent(20));
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Critical, (int)battery_tier_from_percent(19));
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Critical, (int)battery_tier_from_percent(0));
}

void test_power_is_external() {
  TEST_ASSERT_FALSE(power_is_external(PowerSource::Unknown));
  TEST_ASSERT_FALSE(power_is_external(PowerSource::Battery));
  TEST_ASSERT_TRUE(power_is_external(PowerSource::External));
}

// --- BatteryMonitor (smoothing) --------------------------------------------

void test_monitor_no_reading_until_first_sample() {
  BatteryMonitor m;
  TEST_ASSERT_FALSE(m.has_reading());
  TEST_ASSERT_EQUAL_INT(0, m.millivolts());
  TEST_ASSERT_EQUAL_INT(0, m.percent());
}

void test_monitor_first_sample_seeds_directly() {
  BatteryMonitor m(0.2f);
  m.add_tap_sample(1900);  // -> 3800 mV VBAT
  TEST_ASSERT_TRUE(m.has_reading());
  TEST_ASSERT_EQUAL_INT(3800, m.millivolts());
  TEST_ASSERT_EQUAL_INT(50, m.percent());
  TEST_ASSERT_EQUAL_INT((int)BatteryTier::Low, (int)m.tier());
}

void test_monitor_ema_moves_gradually() {
  BatteryMonitor m(0.25f);
  m.add_tap_sample(1900);  // seed 3800 mV
  // Jump the input up; EMA should move only a fraction of the way.
  m.add_tap_sample(2100);  // 4200 mV instantaneous
  // 3800 + 0.25*(4200-3800) = 3900.
  TEST_ASSERT_INT_WITHIN(1, 3900, m.millivolts());
  // Feed the same high value repeatedly; converges toward 4200.
  for (int i = 0; i < 50; ++i) m.add_tap_sample(2100);
  TEST_ASSERT_INT_WITHIN(2, 4200, m.millivolts());
  TEST_ASSERT_EQUAL_INT(100, m.percent());
}

void test_monitor_reset() {
  BatteryMonitor m;
  m.add_tap_sample(2000);
  TEST_ASSERT_TRUE(m.has_reading());
  m.reset();
  TEST_ASSERT_FALSE(m.has_reading());
  TEST_ASSERT_EQUAL_INT(0, m.millivolts());
}

void test_monitor_alpha_one_is_passthrough() {
  BatteryMonitor m(1.0f);
  m.add_tap_sample(1900);  // 3800
  m.add_tap_sample(2050);  // 4100, alpha=1 -> tracks fully
  TEST_ASSERT_EQUAL_INT(4100, m.millivolts());
  TEST_ASSERT_EQUAL_INT(90, m.percent());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_vbat_from_tap_mv);
  RUN_TEST(test_percent_endpoints_and_clamps);
  RUN_TEST(test_percent_curve_knots);
  RUN_TEST(test_percent_interpolates_between_knots);
  RUN_TEST(test_percent_is_monotonic_non_decreasing);
  RUN_TEST(test_tier_thresholds);
  RUN_TEST(test_power_is_external);
  RUN_TEST(test_monitor_no_reading_until_first_sample);
  RUN_TEST(test_monitor_first_sample_seeds_directly);
  RUN_TEST(test_monitor_ema_moves_gradually);
  RUN_TEST(test_monitor_reset);
  RUN_TEST(test_monitor_alpha_one_is_passthrough);
  return UNITY_END();
}
