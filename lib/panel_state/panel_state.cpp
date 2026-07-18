#include "panel_state.h"

#include <algorithm>
#include <string>

namespace osp {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PanelState::PanelState(StationModel& model, OsClient& client,
                       int default_run_time_s)
    : model_(model), client_(client) {
  view_.run_time_s = clamp_run_time(default_run_time_s);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int PanelState::clamp_run_time(int t_sec) const {
  // Round down to nearest step, then clamp.
  t_sec = (t_sec / kRunTimeStep) * kRunTimeStep;
  if (t_sec < kMinRunTime) return kMinRunTime;
  if (t_sec > kMaxRunTime) return kMaxRunTime;
  return t_sec;
}

void PanelState::post_toast(const std::string& msg) {
  view_.toast = msg;
  toast_set_ms_ = now_ms_;
}

void PanelState::go_idle(const std::string& toast) {
  view_.phase = Phase::Idle;
  view_.running_sid = -1;
  view_.countdown_s = 0;
  // Reset the idle timer so sleep doesn't fire immediately on transition.
  last_touch_ms_ = now_ms_;
  if (!toast.empty()) post_toast(toast);
}

void PanelState::go_running(int sid, int t_sec) {
  view_.phase = Phase::Running;
  view_.running_sid = sid;
  view_.countdown_s = t_sec;
  view_.sleeping = false;
  last_countdown_tick_ms_ = now_ms_;
}

void PanelState::on_station_expired() {
  // Countdown reached zero via the panel's own timer — natural expiry.
  const int from_sid = view_.running_sid;
  if (view_.auto_advance) {
    const int next = model_.auto_next_sid(from_sid);
    if (next == -1) {
      // Last station: stop, do not loop.
      go_idle("Finished all stations.");
    } else {
      go_running(next, view_.run_time_s);
      client_.advance(from_sid, next, view_.run_time_s);
    }
  } else {
    go_idle("Station " + std::to_string(station_number(from_sid)) + " finished.");
  }
}

// ---------------------------------------------------------------------------
// tick — called from the firmware loop (e.g. every 10–20 ms on Arduino)
// ---------------------------------------------------------------------------

bool PanelState::tick(uint32_t now_ms) {
  if (!initialized_) {
    initialized_ = true;
    now_ms_ = now_ms;
    last_countdown_tick_ms_ = now_ms;
    last_touch_ms_ = now_ms;
    last_poll_trigger_ms_ = now_ms;
    // Trigger an immediate poll on first tick so the UI reflects reality ASAP.
    return true;
  }

  now_ms_ = now_ms;

  // Clear expired toast.
  if (!view_.toast.empty() && (now_ms - toast_set_ms_ >= kToastDurationMs)) {
    view_.toast.clear();
  }

  // Countdown tick: decrement once per elapsed second while running.
  if (view_.phase == Phase::Running && view_.countdown_s > 0) {
    const uint32_t elapsed = now_ms - last_countdown_tick_ms_;
    const uint32_t secs = elapsed / 1000u;
    if (secs > 0u) {
      last_countdown_tick_ms_ += secs * 1000u;
      // countdown_s > 0 is guaranteed by the outer guard above.
      if (secs >= static_cast<uint32_t>(view_.countdown_s)) {
        view_.countdown_s = 0;
        on_station_expired();
      } else {
        view_.countdown_s -= static_cast<int>(secs);
      }
    }
  }

  // Sleep timer: blank the screen after kSleepTimeoutMs of idle-and-untouched.
  // Never sleep while a station is running.
  if (view_.phase == Phase::Idle && !view_.sleeping) {
    if ((now_ms - last_touch_ms_) >= kSleepTimeoutMs) {
      view_.sleeping = true;
    }
  }

  // Poll scheduling: return true once per kPollIntervalMs.
  if ((now_ms - last_poll_trigger_ms_) >= kPollIntervalMs) {
    last_poll_trigger_ms_ = now_ms;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// on_jc — /jc poll result (controller is source of truth)
// ---------------------------------------------------------------------------

void PanelState::on_jc(const JcData& jc) {
  view_.connected = true;
  view_.ctrl_rssi = jc.rssi;

  // Determine which station (if any) the controller says is active.
  // A station is considered running if its sbits bit is set AND ps[sid].rem > 0.
  // Skip non-runnable sids (disabled/master) so they are never mistaken for the
  // active station even if the controller reports a stray sbit/rem.
  int jc_running_sid = -1;
  for (int sid = 0; sid < static_cast<int>(jc.ps.size()); ++sid) {
    if (model_.runnable_index(sid) == -1) continue;
    if (board_bit_set(jc.sbits, sid) && jc.ps[sid].rem > 0) {
      jc_running_sid = sid;
      break;
    }
  }

  if (jc_running_sid != -1) {
    // Controller reports a station on — update/correct the panel state.
    if (view_.phase != Phase::Running ||
        view_.running_sid != jc_running_sid) {
      // Different or unexpected station (e.g. externally started): adopt it.
      view_.phase = Phase::Running;
      view_.running_sid = jc_running_sid;
      view_.sleeping = false;
    }
    // Reconcile countdown from the controller's authoritative `rem`.
    view_.countdown_s = jc.ps[jc_running_sid].rem;
    last_countdown_tick_ms_ = now_ms_;
  } else {
    // Controller shows nothing running.
    if (view_.phase == Phase::Running) {
      // Station disappeared from the controller without our stop command:
      // it was stopped externally or finished while we were between polls.
      // The panel's own timer drives auto-advance; the poll only reconciles.
      go_idle("Station " + std::to_string(station_number(view_.running_sid)) +
              " finished.");
    }
    // else: already idle — no state change needed.
  }
}

// ---------------------------------------------------------------------------
// on_jc_error — transport failure / timeout
// ---------------------------------------------------------------------------

void PanelState::on_jc_error() {
  view_.connected = false;
  // Do NOT change the running/idle state: the station on the controller will
  // auto-stop at its RT. State reconciles when connectivity restores.
}

// ---------------------------------------------------------------------------
// User actions
// ---------------------------------------------------------------------------

void PanelState::on_touch(uint32_t now_ms) {
  last_touch_ms_ = now_ms;
  view_.sleeping = false;
}

void PanelState::select_station(int sid) {
  on_touch(now_ms_);
  if (model_.runnable_index(sid) == -1) return;  // skip non-runnable

  if (view_.phase == Phase::Running) {
    // Jump: turn current off, target on (off-then-on via advance).
    const int from = view_.running_sid;
    if (from == sid) return;  // already on this station — no-op
    go_running(sid, view_.run_time_s);
    client_.advance(from, sid, view_.run_time_s);
  } else {
    // Idle → start this station.
    go_running(sid, view_.run_time_s);
    client_.run_station(sid, view_.run_time_s);
  }
}

void PanelState::advance() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);

  const int next = model_.next_sid(view_.running_sid);
  if (next == -1) return;  // no runnable stations

  const int from = view_.running_sid;
  go_running(next, view_.run_time_s);
  client_.advance(from, next, view_.run_time_s);
}

void PanelState::prev() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);

  const int prv = model_.prev_sid(view_.running_sid);
  if (prv == -1) return;  // no runnable stations

  const int from = view_.running_sid;
  go_running(prv, view_.run_time_s);
  client_.advance(from, prv, view_.run_time_s);
}

void PanelState::stop() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);

  go_idle("Stopped.");
  client_.stop_all();
}

void PanelState::set_run_time(int t_sec) {
  on_touch(now_ms_);
  view_.run_time_s = clamp_run_time(t_sec);

  if (view_.phase == Phase::Running) {
    // Extend: restart the current station at the new run time (off-then-on).
    view_.countdown_s = view_.run_time_s;
    last_countdown_tick_ms_ = now_ms_;
    client_.extend(view_.running_sid, view_.run_time_s);
  }
}

void PanelState::set_auto_advance(bool enabled) {
  on_touch(now_ms_);
  view_.auto_advance = enabled;
}

}  // namespace osp
