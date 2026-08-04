#include "battery_monitor.h"

#include <algorithm>
#include <cmath>

namespace osp {

bool power_is_external(PowerSource source) {
  return source == PowerSource::External;
}

int battery_vbat_from_tap_mv(int tap_mv) {
  if (tap_mv < 0) tap_mv = 0;
  return static_cast<int>(tap_mv * kBatteryDividerRatio + 0.5f);
}

namespace {

// Piecewise-linear LiPo discharge curve: (VBAT mV, % charge), descending by
// voltage. Coarse on purpose — this drives a wall glyph, not fuel gauging. The
// knee below ~3.7 V (where LiPo voltage falls off fast) is captured by the
// tighter spacing there.
struct CurvePoint {
  int mv;
  int pct;
};
constexpr CurvePoint kCurve[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
    {3700, 35},  {3600, 20}, {3500, 12}, {3400, 8},  {3300, 5},
    {3000, 0},
};
constexpr int kCurveLen = static_cast<int>(sizeof(kCurve) / sizeof(kCurve[0]));

}  // namespace

int battery_percent_from_mv(int vbat_mv) {
  if (vbat_mv >= kCurve[0].mv) return kCurve[0].pct;               // >= full
  if (vbat_mv <= kCurve[kCurveLen - 1].mv) return kCurve[kCurveLen - 1].pct;  // <= empty
  // Find the segment [hi, lo] straddling vbat_mv (curve descends by voltage).
  for (int i = 0; i < kCurveLen - 1; ++i) {
    const CurvePoint& hi = kCurve[i];
    const CurvePoint& lo = kCurve[i + 1];
    if (vbat_mv <= hi.mv && vbat_mv >= lo.mv) {
      const int dv = hi.mv - lo.mv;    // > 0
      const int dp = hi.pct - lo.pct;  // >= 0
      // Linear interpolation, rounded.
      return lo.pct + (dp * (vbat_mv - lo.mv) + dv / 2) / dv;
    }
  }
  return 0;  // unreachable given the clamps above
}

BatteryTier battery_tier_from_percent(int percent) {
  if (percent > 50) return BatteryTier::Healthy;
  if (percent >= 20) return BatteryTier::Low;
  return BatteryTier::Critical;
}

BatteryMonitor::BatteryMonitor(float alpha) {
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;
  alpha_ = alpha;
}

void BatteryMonitor::reset() {
  vbat_mv_ = 0.0f;
  has_reading_ = false;
}

void BatteryMonitor::add_tap_sample(int tap_mv) {
  const float vbat = static_cast<float>(battery_vbat_from_tap_mv(tap_mv));
  if (!has_reading_) {
    vbat_mv_ = vbat;
    has_reading_ = true;
  } else {
    vbat_mv_ += alpha_ * (vbat - vbat_mv_);
  }
}

int BatteryMonitor::millivolts() const {
  if (!has_reading_) return 0;
  return static_cast<int>(vbat_mv_ + 0.5f);
}

int BatteryMonitor::percent() const {
  if (!has_reading_) return 0;
  return battery_percent_from_mv(millivolts());
}

}  // namespace osp
