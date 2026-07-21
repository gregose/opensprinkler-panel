// Hardware-independent LiPo battery monitor for the CYD panel.
//
// Everything here is pure C++ (no Arduino, no ADC) so it builds and runs under
// the PlatformIO `native` test environment. The Arduino glue in src/ samples
// the ADC (GPIO34 = BAT_ADC) and feeds calibrated tap-node millivolts in here;
// this maps them to a battery voltage, a coarse state-of-charge %, a colour
// tier, and applies exponential smoothing.
//
// Hardware facts this encodes (schematic E32R35T, bench-confirmed):
//   - BAT_ADC senses VBAT through an even 100K/100K divider, so
//     VBAT = tap_mV * 2 (ratio confirmed 1.991 on-device; using 2.0).
//   - There is NO charger-status GPIO, and an *absent* battery floats the node
//     to ~4.1 V (TP4054 no-load). Absence is therefore indistinguishable from a
//     full cell, so we ALWAYS report a reading and never a "no battery" state.
//     A genuinely low reading (< ~3.0 V) only ever means a flat cell.
#pragma once

#include <cstdint>

namespace osp {

// Resistor-divider ratio on BAT_ADC: VBAT = tap_mV * kBatteryDividerRatio.
inline constexpr float kBatteryDividerRatio = 2.0f;

// LiPo discharge-curve endpoints used for the %-of-charge mapping. VBAT is
// clamped to this window before interpolation.
inline constexpr int kBatteryEmptyMv = 3000;  // 0 %
inline constexpr int kBatteryFullMv  = 4200;  // 100 %

// Colour tiers for the top-bar glyph (map to teal / amber / red).
enum class BatteryTier { Healthy, Low, Critical };

// Convert a calibrated tap-node reading (mV at the ADC pin) to VBAT (mV).
int battery_vbat_from_tap_mv(int tap_mv);

// Map a battery voltage (mV) to a coarse state-of-charge percentage (0..100),
// via a piecewise-linear LiPo discharge curve. Clamped at both ends.
int battery_percent_from_mv(int vbat_mv);

// State-of-charge percentage -> colour tier: >50 Healthy, 20..50 Low,
// <20 Critical.
BatteryTier battery_tier_from_percent(int percent);

// Stateful smoother. Feed calibrated tap-node millivolts as they are sampled;
// read back the smoothed VBAT / percent / tier. Exponential moving average so a
// single noisy ADC frame can't jump the on-screen gauge.
class BatteryMonitor {
 public:
  // `alpha` is the EMA weight for each new sample in [0,1]; larger = snappier,
  // smaller = smoother. The default is a calm gauge for a wall panel.
  explicit BatteryMonitor(float alpha = 0.2f);

  // Feed one calibrated tap-node reading (mV at the ADC pin). The first sample
  // seeds the filter directly (no ramp-up from zero).
  void add_tap_sample(int tap_mv);

  // Reset to the pre-first-sample state.
  void reset();

  bool has_reading() const { return has_reading_; }

  // Smoothed VBAT in millivolts (0 before the first sample).
  int millivolts() const;

  // Smoothed state-of-charge percentage 0..100 (0 before the first sample).
  int percent() const;

  // Colour tier for the smoothed percentage.
  BatteryTier tier() const { return battery_tier_from_percent(percent()); }

 private:
  float alpha_;
  float vbat_mv_ = 0.0f;
  bool has_reading_ = false;
};

}  // namespace osp
