// Hardware-independent UI state machine for the OpenSprinkler station panel.
//
// Pure C++ (no Arduino, no LVGL, no network) so it builds and runs under the
// PlatformIO `native` test environment. It composes `StationModel` (navigation
// and layout) and `OsClient` (commands, via its injected `Transport`) to drive
// the single-screen UX defined in docs/01-ux-spec.md.
//
// Key design principles (from docs/01 + the M6 kickoff comment):
//   - tick(now_ms) and on_jc(JcData) are the only time/poll inputs, making the
//     state machine deterministic and fully unit-testable without hardware.
//   - select = run: tapping a station always starts it immediately.
//   - Advance/Prev: manual stepping wraps using StationModel::next_sid /
//     prev_sid (which already skip disabled/master stations).
//   - Jump: tapping a station while running uses OsClient::advance (off→on).
//   - Run-time stepper: changing RT while running extends via OsClient::extend.
//   - Auto-advance: on natural expiry (countdown→0 in tick), auto_next_sid
//     advances; stops after the last station (no wrap). Manual Advance wraps.
//   - /jc reconcile: on_jc is the controller truth — updates running_sid and
//     countdown, detects externally-stopped stations.
//   - Signal loss: on_jc_error sets connected=false; on_jc recovery sets true.
//   - 5-min idle sleep: modelled here; actual backlight PWM is src/ glue.
#pragma once

#include <cstdint>
#include <string>

#include "os_client.h"
#include "station_model.h"

namespace osp {

// ---------------------------------------------------------------------------
// Phase — the two visible screen states
// ---------------------------------------------------------------------------
enum class Phase { Idle, Running };

// ---------------------------------------------------------------------------
// PanelView — all display-relevant state (read by the UI layer)
// ---------------------------------------------------------------------------
struct PanelView {
  Phase phase = Phase::Idle;
  bool sleeping = false;       // backlight-off overlay active
  bool connected = true;       // controller reachable
  int running_sid = -1;        // 0-based; -1 = nothing running
  int countdown_s = 0;         // seconds remaining on current station
  int run_time_s = 60;         // current run-time setting (seconds)
  bool auto_advance = false;   // auto-advance toggle state
  int ctrl_rssi = 0;           // controller Wi-Fi RSSI dBm (0 = unknown)
  std::string toast;           // transient message ("" = none)
};

// ---------------------------------------------------------------------------
// PanelState — the state machine
// ---------------------------------------------------------------------------
class PanelState {
 public:
  static constexpr int kDefaultRunTime = 60;       // seconds
  static constexpr int kMinRunTime = 15;
  static constexpr int kMaxRunTime = 600;
  static constexpr int kRunTimeStep = 15;
  static constexpr uint32_t kPollIntervalMs = 2000;
  static constexpr uint32_t kSleepTimeoutMs = 300000;  // 5 minutes
  static constexpr uint32_t kToastDurationMs = 3000;

  PanelState(StationModel& model, OsClient& client,
             int default_run_time_s = kDefaultRunTime);

  // ---------------------------------------------------------------------------
  // Time / poll inputs (drive via the firmware loop; injected in tests)
  // ---------------------------------------------------------------------------

  // Advance time. Returns true when the caller should issue a /jc poll.
  // now_ms: monotonic millisecond counter (e.g. millis() on Arduino).
  bool tick(uint32_t now_ms);

  // Feed the result of a successful /jc poll into the state machine.
  // Called by the driver immediately after OsClient::fetch_jc returns true.
  void on_jc(const JcData& jc);

  // Signal a /jc poll failure (transport error or parse failure).
  void on_jc_error();

  // ---------------------------------------------------------------------------
  // User actions — each one also resets the idle/sleep timer
  // ---------------------------------------------------------------------------

  // Tap a station pill: start that station (or jump to it if already running).
  void select_station(int sid);

  // Advance › button: next station (wraps, skips non-runnable).
  void advance();

  // ‹ Prev button: previous station (wraps, skips non-runnable).
  void prev();

  // ■ Stop button: stop everything immediately.
  void stop();

  // Run-time −/+ stepper: change RT. Restarts current station if running.
  // t_sec is clamped to [kMinRunTime, kMaxRunTime] and rounded to kRunTimeStep.
  void set_run_time(int t_sec);

  // Auto-advance toggle.
  void set_auto_advance(bool enabled);

  // Any touch: wakes from sleep, resets idle timer.
  void on_touch(uint32_t now_ms);

  // ---------------------------------------------------------------------------
  // Read-only view (called by the UI layer each frame)
  // ---------------------------------------------------------------------------
  const PanelView& view() const { return view_; }

 private:
  StationModel& model_;
  OsClient& client_;
  PanelView view_;

  uint32_t now_ms_ = 0;
  uint32_t last_countdown_tick_ms_ = 0;
  uint32_t last_touch_ms_ = 0;
  uint32_t last_poll_trigger_ms_ = 0;
  uint32_t toast_set_ms_ = 0;
  bool initialized_ = false;

  // Helpers
  void go_idle(const std::string& toast = "");
  void go_running(int sid, int t_sec);
  void on_station_expired();
  void post_toast(const std::string& msg);
  int clamp_run_time(int t_sec) const;

  // 1-based station display number (0-based sid + 1).
  static int station_number(int sid) { return sid + 1; }
};

}  // namespace osp
