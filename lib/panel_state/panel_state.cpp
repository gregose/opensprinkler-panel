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
  run_initiated_by_panel_ = true;
  view_.run_initiated_by_panel = true;
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

void PanelState::enter_program_running(const ProgramRunState& prog_state) {
  const bool newly_entered = (view_.phase != Phase::ProgramRunning);
  view_.phase = Phase::ProgramRunning;
  view_.prog_run = prog_state;
  // Sync the countdown from the current station for dead-reckoning between polls.
  int cd = 0;
  if (view_.prog_run.current_sid >= 0) {
    for (const auto& q : view_.prog_run.queue) {
      if (q.sid == view_.prog_run.current_sid) {
        cd = q.remaining_seconds;
        break;
      }
    }
  } else if (!view_.prog_run.queue.empty()) {
    // No station reports as "started" — this happens while the program is
    // paused (the controller pushes every station's start time into the
    // future) or in the brief gap between stations. Fall back to the first
    // station that still has time left so the running screen shows a station
    // name + its countdown instead of a blank 0:00 "Finishing..." state.
    const ProgramQueueEntry* pick = nullptr;
    int pick_number = 0;
    int n = 0;
    for (const auto& q : view_.prog_run.queue) {
      ++n;
      if (q.remaining_seconds > 0) {
        pick = &q;
        pick_number = n;
        break;
      }
    }
    if (pick == nullptr) {
      pick = &view_.prog_run.queue.front();
      pick_number = 1;
    }
    view_.prog_run.current_sid = pick->sid;
    view_.prog_run.current_station_number = pick_number;
    cd = pick->remaining_seconds;
  }
  view_.countdown_s = std::max(0, cd);
  last_countdown_tick_ms_ = now_ms_;
  // A manual program run reports pid=254 in /jc, so program_model can't know
  // which program it is (program_index=-1). If the panel launched it, restore
  // the remembered index so the UI can label the running screen.
  if (view_.prog_run.program_index < 0 && run_initiated_by_panel_ &&
      launched_program_index_ >= 0) {
    view_.prog_run.program_index = launched_program_index_;
  }
  // Only force-wake the display if the panel itself initiated this run.
  // External/scheduled runs let the normal sleep timeout apply.
  if (newly_entered && run_initiated_by_panel_) {
    view_.sleeping = false;
  }
  // Close the programs list screen when a run starts.
  view_.showing_programs_list = false;
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
  view_.prog_run = ProgramRunState{};
  launched_program_index_ = -1;
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

void PanelState::set_program_list(const JpData& jp) {
  jp_cache_ = jp;
}

void PanelState::open_programs_list() {
  on_touch(now_ms_);
  if (view_.phase == Phase::Idle) {
    view_.showing_programs_list = true;
    view_.prog_list_page = 0;
  }
}

void PanelState::close_programs_list() {
  on_touch(now_ms_);
  view_.showing_programs_list = false;
}

void PanelState::set_prog_list_page(int page) {
  on_touch(now_ms_);
  const int nprogs = static_cast<int>(jp_cache_.programs.size());
  const int max_page = (nprogs > 0) ? ((nprogs - 1) / 4) : 0;
  if (page < 0) page = 0;
  if (page > max_page) page = max_page;
  view_.prog_list_page = page;
}

void PanelState::run_program_intent(int pid) {
  on_touch(now_ms_);
  desired_.kind = IntentKind::RunProgram;
  desired_.sid = pid;  // 0-based program index (as required by /mp?pid=)
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
  run_initiated_by_panel_ = true;
  view_.run_initiated_by_panel = true;
  view_.showing_programs_list = false;
  // Remember what we launched so the running screen can show the program name
  // even though a manual program run reports pid=254 (program_index=-1) in /jc.
  launched_program_index_ = pid;
}

void PanelState::toggle_program_enabled_intent(int pid, bool en) {
  on_touch(now_ms_);
  desired_.kind = IntentKind::SetProgramEnabled;
  desired_.sid = pid;  // 0-based program index (as required by /cp?pid=)
  desired_.seconds = en ? 1 : 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
}

void PanelState::pause_toggle_intent() {
  on_touch(now_ms_);
  desired_.kind = IntentKind::Pause;
  desired_.sid = -1;
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
}

void PanelState::program_advance_intent() {
  if (view_.phase != Phase::ProgramRunning) return;
  if (view_.prog_run.current_sid < 0) return;
  on_touch(now_ms_);
  desired_.kind = IntentKind::ProgramAdvance;
  desired_.sid = view_.prog_run.current_sid;
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
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

  // Countdown dead-reckoning for both manual and program running phases.
  if ((view_.phase == Phase::Running || view_.phase == Phase::ProgramRunning) &&
      view_.countdown_s > 0) {
    if (view_.paused) {
      // A paused program freezes its countdown; keep the tick baseline current
      // so resuming doesn't subtract the whole paused interval at once.
      last_countdown_tick_ms_ = now_ms;
    } else {
      const uint32_t elapsed = now_ms - last_countdown_tick_ms_;
      const uint32_t secs = elapsed / 1000u;
      if (secs > 0u) {
        last_countdown_tick_ms_ += secs * 1000u;
        if (secs >= static_cast<uint32_t>(view_.countdown_s)) {
          view_.countdown_s = 0;
          if (view_.phase == Phase::Running && !await_close_) {
            const int finished_sid = view_.running_sid;
            const int next_sid =
                view_.auto_advance ? model_.auto_next_sid(finished_sid) : -1;
            if (next_sid != -1) {
              queue_desired_run(next_sid);
              begin_await_close();
            } else {
              begin_await_close();
            }
          }
          // For ProgramRunning: countdown reaching 0 just holds at 0 until
          // the next /jc poll updates the state.
        } else {
          view_.countdown_s -= static_cast<int>(secs);
        }
      }
    }
  }

  // Sleep timer: applies when idle, and also for external program runs.
  if (should_allow_sleep() && !view_.sleeping) {
    if (sleep_timeout_ms_ != 0 &&
        (now_ms - last_touch_ms_) >= sleep_timeout_ms_) {
      view_.sleeping = true;
    }
  }
}

bool PanelState::should_allow_sleep() const {
  // Idle: always allow sleep.
  if (view_.phase == Phase::Idle) return true;
  // Externally-started program run: allow sleep (backlight can blank while
  // scheduled programs run unattended). Panel-initiated runs keep the
  // backlight on (user is present at the panel).
  if (view_.phase == Phase::ProgramRunning) return !run_initiated_by_panel_;
  // Manual station run or any other phase: do not sleep.
  return false;
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

  if (desired_.kind == IntentKind::RunProgram) {
    // Confirmed once the controller reports a program is running.
    return view_.phase == Phase::ProgramRunning;
  }

  // SetProgramEnabled, Pause, and ProgramAdvance are confirmed as soon as
  // delivered; the network task calls mark_desired_delivered(), then the
  // next reconcile_desired_after_jc() clears them.
  if (desired_.kind == IntentKind::SetProgramEnabled ||
      desired_.kind == IntentKind::Pause ||
      desired_.kind == IntentKind::ProgramAdvance) {
    return desired_delivered_;
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
  view_.sunrise_min = jc.sunrise;
  view_.sunset_min = jc.sunset;
  view_.ctrl_devt = static_cast<long>(jc.devt);
  view_.paused = (jc.pq != 0);
  view_.pause_remaining_s = jc.pt;

  // Classify the controller's current run state using the program model.
  const auto ps = to_program_ps(jc);
  ProgramRunState prog_state =
      resolve_program_run_state(ps, jp_cache_.nprogs, static_cast<long>(jc.devt));

  // If a program is running, re-resolve with the program's *full* ordered
  // station list (from /jp) so the queue includes already-completed stations.
  // This makes "STATION N OF M" count up through a fixed M and lets finished
  // stations render as done instead of vanishing. A panel-launched run reports
  // pid=254 (program_index=-1); fall back to the remembered launched index.
  if (prog_state.run_class == RunClass::ProgramRun) {
    int eff_idx = prog_state.program_index;
    if (eff_idx < 0 && run_initiated_by_panel_ && launched_program_index_ >= 0) {
      eff_idx = launched_program_index_;
    }
    if (eff_idx >= 0 && eff_idx < static_cast<int>(jp_cache_.programs.size())) {
      const auto& durs = jp_cache_.programs[eff_idx].durations;
      std::vector<ProgramStation> pstations;
      for (int sid = 0; sid < static_cast<int>(durs.size()); ++sid) {
        if (durs[sid] > 0) pstations.push_back(ProgramStation{sid, durs[sid]});
      }
      if (!pstations.empty()) {
        prog_state = resolve_program_run_state(
            ps, jp_cache_.nprogs, static_cast<long>(jc.devt), &pstations);
        prog_state.program_index = eff_idx;  // keep name resolvable for pid=254
      }
    }
  }

  // Find the running manual-station sid (needed only for ManualRun path).
  int jc_running_sid = -1;
  int jc_running_rem = 0;
  if (prog_state.run_class == RunClass::ManualRun) {
    for (int sid = 0; sid < static_cast<int>(jc.ps.size()); ++sid) {
      if (model_.runnable_index(sid) == -1) continue;
      if (board_bit_set(jc.sbits, sid) && jc.ps[sid].rem > 0) {
        jc_running_sid = sid;
        jc_running_rem = jc.ps[sid].rem;
        break;
      }
    }
  }

  if (prog_state.run_class == RunClass::ProgramRun) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_program_running(prog_state);
  } else if (jc_running_sid != -1) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_running(jc_running_sid, jc_running_rem);
  } else {
    // Idle: clear run_initiated flag so backlight/sleep behave normally.
    run_initiated_by_panel_ = false;
    view_.run_initiated_by_panel = false;
    if (await_close_) {
      finish_idle_transition();
    } else if (desired_.kind == IntentKind::Stop) {
      enter_idle();
      clear_desired();
      return;
    } else if (has_desired()) {
      enter_idle();
    } else if (view_.phase == Phase::Running ||
               view_.phase == Phase::ProgramRunning) {
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
  if (view_.phase != Phase::Running && view_.phase != Phase::ProgramRunning) return;
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
