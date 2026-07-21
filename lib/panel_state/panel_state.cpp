#include "panel_state.h"

#include <algorithm>
#include <string>

namespace osp {

PanelState::PanelState(StationModel& model, int default_run_time_s)
    : model_(model) {
  view_.run_time_s = clamp_run_time(default_run_time_s);
}

int PanelState::clamp_run_time(int t_sec) const {
  t_sec = (t_sec / kRunTimeStep) * kRunTimeStep;
  if (t_sec < kMinRunTime) return kMinRunTime;
  if (t_sec > kMaxRunTime) return kMaxRunTime;
  return t_sec;
}

void PanelState::clear_desired() {
  desired_ = DesiredIntent{};
  desired_delivered_ = false;
  desired_at_ms_ = 0;
}

void PanelState::queue_desired_run(int sid) {
  desired_.kind = IntentKind::Run;
  desired_.sid = sid;
  desired_.seconds = view_.run_time_s;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
}

void PanelState::queue_desired_stop() {
  desired_.kind = IntentKind::Stop;
  desired_.sid = -1;
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
}

void PanelState::enter_running(int sid, int countdown_s) {
  view_.phase = Phase::Running;
  view_.running_sid = sid;
  view_.countdown_s = std::max(0, countdown_s);
  view_.sleeping = false;
  last_countdown_tick_ms_ = now_ms_;
}

void PanelState::enter_idle() {
  // Only reset the idle-sleep clock on a genuine transition INTO idle
  // (e.g. Running->Idle). on_jc() re-affirms idle on EVERY ~2 s /jc poll
  // while already idle; if we reset here unconditionally the idle timer is
  // wiped every poll and idle_elapsed_ms() can never reach the sleep
  // timeout, so the screen never sleeps (#60).
  const bool was_idle = (view_.phase == Phase::Idle);
  view_.phase = Phase::Idle;
  view_.running_sid = -1;
  view_.countdown_s = 0;
  if (!was_idle) last_touch_ms_ = now_ms_;
}

void PanelState::begin_await_close() {
  await_close_ = true;
  await_close_at_ms_ = now_ms_;
  if (view_.phase == Phase::Running) {
    view_.countdown_s = 0;
  }
}

void PanelState::finish_idle_transition() {
  await_close_ = false;
  await_close_at_ms_ = 0;
  enter_idle();
}

bool PanelState::pending_sync() const {
  return has_desired() || await_close_;
}

bool PanelState::sync_stale() const {
  if (!pending_sync()) return false;
  const uint32_t started_at = await_close_ ? await_close_at_ms_ : desired_at_ms_;
  return (now_ms_ - started_at) > kSyncTimeoutMs;
}

bool PanelState::can_deliver_desired() const {
  return has_desired() && !desired_delivered_ && !await_close_;
}

void PanelState::mark_desired_delivered() { desired_delivered_ = has_desired(); }

void PanelState::mark_desired_needs_retry() {
  if (has_desired()) desired_delivered_ = false;
}

void PanelState::on_link_connected(uint32_t now_ms) {
  now_ms_ = now_ms;
  view_.link = LinkState::Connected;
}

void PanelState::on_link_reconnecting(uint32_t now_ms) {
  now_ms_ = now_ms;
  if (view_.link != LinkState::AuthError) {
    view_.link = LinkState::Reconnecting;
  }
}

void PanelState::on_link_offline(uint32_t now_ms) {
  now_ms_ = now_ms;
  if (view_.link != LinkState::AuthError) {
    view_.link = LinkState::Offline;
  }
}

void PanelState::on_auth_error(uint32_t now_ms) {
  now_ms_ = now_ms;
  view_.link = LinkState::AuthError;
  mark_desired_needs_retry();
}

void PanelState::set_station_list_loaded(bool loaded) {
  view_.station_list_loaded = loaded;
}

void PanelState::tick(uint32_t now_ms) {
  if (!initialized_) {
    initialized_ = true;
    now_ms_ = now_ms;
    last_touch_ms_ = now_ms;
    last_countdown_tick_ms_ = now_ms;
    return;
  }

  now_ms_ = now_ms;

  if (view_.phase == Phase::Running && view_.countdown_s > 0) {
    const uint32_t elapsed = now_ms - last_countdown_tick_ms_;
    const uint32_t secs = elapsed / 1000u;
    if (secs > 0u) {
      last_countdown_tick_ms_ += secs * 1000u;
      if (secs >= static_cast<uint32_t>(view_.countdown_s)) {
        const int finished_sid = view_.running_sid;
        view_.countdown_s = 0;
        if (!await_close_) {
          const int next_sid =
              view_.auto_advance ? model_.auto_next_sid(finished_sid) : -1;
          if (next_sid != -1) {
            queue_desired_run(next_sid);
            begin_await_close();
          } else {
            begin_await_close();
          }
        }
      } else {
        view_.countdown_s -= static_cast<int>(secs);
      }
    }
  }

  if (view_.phase == Phase::Idle && !view_.sleeping) {
    if (sleep_timeout_ms_ != 0 &&
        (now_ms - last_touch_ms_) >= sleep_timeout_ms_) {
      view_.sleeping = true;
    }
  }
}

void PanelState::reconcile_desired_after_jc() {
  if (!has_desired()) return;
  if (desired_matches_confirmed()) {
    clear_desired();
  } else {
    desired_delivered_ = false;
  }
}

bool PanelState::desired_matches_confirmed() const {
  if (!has_desired()) return false;

  if (desired_.kind == IntentKind::Stop) {
    return view_.phase == Phase::Idle && !await_close_;
  }

  if (view_.phase != Phase::Running) return false;
  if (view_.running_sid != desired_.sid) return false;

  const int min_confirmed =
      std::max(0, desired_.seconds - kConfirmGraceSeconds);
  return view_.countdown_s >= min_confirmed && view_.countdown_s <= desired_.seconds;
}

void PanelState::on_jc(const JcData& jc, uint32_t now_ms) {
  now_ms_ = now_ms;
  on_link_connected(now_ms);
  view_.ctrl_rssi = jc.rssi;

  int jc_running_sid = -1;
  int jc_running_rem = 0;
  for (int sid = 0; sid < static_cast<int>(jc.ps.size()); ++sid) {
    if (model_.runnable_index(sid) == -1) continue;
    if (board_bit_set(jc.sbits, sid) && jc.ps[sid].rem > 0) {
      jc_running_sid = sid;
      jc_running_rem = jc.ps[sid].rem;
      break;
    }
  }

  if (jc_running_sid != -1) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_running(jc_running_sid, jc_running_rem);
  } else {
    if (await_close_) {
      finish_idle_transition();
    } else if (desired_.kind == IntentKind::Stop) {
      enter_idle();
      clear_desired();
      return;
    } else if (has_desired()) {
      enter_idle();
    } else if (view_.phase == Phase::Running) {
      finish_idle_transition();
    } else {
      enter_idle();
    }
  }

  reconcile_desired_after_jc();
}

void PanelState::on_touch(uint32_t now_ms) {
  now_ms_ = now_ms;
  last_touch_ms_ = now_ms;
  view_.sleeping = false;
}

void PanelState::select_station(int sid) {
  on_touch(now_ms_);
  if (model_.runnable_index(sid) == -1) return;
  if (view_.phase == Phase::Running && view_.running_sid == sid &&
      !await_close_ && !has_desired()) {
    return;
  }
  queue_desired_run(sid);
}

void PanelState::advance() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);
  const int sid = model_.next_sid(view_.running_sid);
  if (sid != -1) queue_desired_run(sid);
}

void PanelState::prev() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);
  const int sid = model_.prev_sid(view_.running_sid);
  if (sid != -1) queue_desired_run(sid);
}

void PanelState::stop() {
  if (view_.phase != Phase::Running) return;
  on_touch(now_ms_);
  queue_desired_stop();
}

void PanelState::set_run_time(int t_sec) {
  on_touch(now_ms_);
  view_.run_time_s = clamp_run_time(t_sec);
}

void PanelState::set_auto_advance(bool enabled) {
  on_touch(now_ms_);
  view_.auto_advance = enabled;
}

}  // namespace osp
