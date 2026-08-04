#include "panel_state.h"

#include <algorithm>
#include <limits>
#include <string>

namespace osp {

namespace {
// True when every station currently queued for `pid` in the live ps[] belongs
// to program `prog_idx`'s definition (and at least one such station exists).
// Used to decide whether a panel-launched program index still describes the
// live run: while a run drains toward its final station(s) the panel-remembered
// index is authoritative, but if the live set no longer fits that program
// (e.g. the launched run ended and a different pid=254 run has started) the
// hint must be dropped rather than mislabelling the new run.
bool live_stations_in_program(const JpData& jp,
                              const std::vector<ProgramPsEntry>& ps,
                              int pid, int prog_idx) {
  if (prog_idx < 0 || prog_idx >= static_cast<int>(jp.programs.size())) {
    return false;
  }
  const auto& durs = jp.programs[prog_idx].durations;
  bool any = false;
  for (int sid = 0; sid < static_cast<int>(ps.size()); ++sid) {
    if (ps[sid].pid != pid) continue;
    any = true;
    if (!(sid < static_cast<int>(durs.size()) && durs[sid] > 0)) return false;
  }
  return any;
}
}  // namespace

std::string format_rain_delay(int seconds_remaining) {
  if (seconds_remaining < 3600) return "<1h";

  const int hours = seconds_remaining / 3600;
  const int days = hours / 24;
  const int remaining_hours = hours % 24;
  if (days == 0) return std::to_string(hours) + "h";
  if (remaining_hours == 0) return std::to_string(days) + "d";
  return std::to_string(days) + "d " + std::to_string(remaining_hours) + "h";
}

TopBarState resolve_top_bar_state(const PanelView& view) {
  if (view.show_syncing) return TopBarState::Syncing;
  if (view.link == LinkState::AuthError) return TopBarState::AuthError;
  if (view.link == LinkState::Reconnecting) return TopBarState::Reconnecting;
  if (view.link == LinkState::Offline) return TopBarState::Offline;
  if (!view.enabled) return TopBarState::Disabled;
  if (view.rain_delay_seconds_remaining > 0) {
    return TopBarState::RainDelay;
  }
  // A paused run (manual or program) is NOT surfaced in the top bar: the
  // on-panel two-line PAUSED block is the sole paused indicator, so the top bar
  // keeps reflecting the underlying connection/enabled/rain-delay state with no
  // extra amber cue.
  return TopBarState::Clean;
}

std::string top_bar_status_text(const PanelView& view) {
  switch (resolve_top_bar_state(view)) {
    case TopBarState::Syncing:
      return "SYNCING...";
    case TopBarState::AuthError:
      return "AUTH ERROR";
    case TopBarState::Reconnecting:
      return "RECONNECTING...";
    case TopBarState::Offline:
      return "OFFLINE";
    case TopBarState::Disabled:
      return "DISABLED";
    case TopBarState::RainDelay: {
      std::string duration =
          format_rain_delay(view.rain_delay_seconds_remaining);
      for (char& ch : duration) {
        if (ch == 'd' || ch == 'h') ch -= ('a' - 'A');
      }
      return "RAIN DELAY " + duration;
    }
    case TopBarState::Clean:
      return "";
  }
  return "";
}

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
  update_sync_view();
}

// #143: start (or re-affirm) a panel-manual session. Records the launched sid as
// the session identity and anchors the hysteresis grace at now. Called from
// queue_desired_run(), so it also covers advance/prev/reselect and every
// auto-advance target the tick loop queues.
void PanelState::begin_panel_manual_session(int sid) {
  panel_manual_active_ = true;
  panel_manual_sid_ = sid;
  panel_manual_seen_ms_ = now_ms_;
}

void PanelState::end_panel_manual_session() {
  panel_manual_active_ = false;
  panel_manual_sid_ = -1;
}

void PanelState::queue_desired_run(int sid) {
  desired_.kind = IntentKind::Run;
  desired_.sid = sid;
  desired_.seconds = view_.run_time_s;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
  run_initiated_by_panel_ = true;
  view_.run_initiated_by_panel = true;
  begin_panel_manual_session(sid);
  update_sync_view();
}

void PanelState::queue_desired_stop() {
  desired_.kind = IntentKind::Stop;
  desired_.sid = -1;
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
  update_sync_view();
}

void PanelState::enter_running(int sid, int countdown_s) {
  view_.phase = Phase::Running;
  view_.running_sid = sid;
  view_.countdown_s = std::max(0, countdown_s);
  view_.sleeping = false;
  last_countdown_tick_ms_ = now_ms_;
  // Close the programs list / history overlays when a run starts.
  view_.showing_programs_list = false;
  view_.showing_history = false;
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
  // Note: program_index is resolved authoritatively in on_jc() before this
  // runs (real pid -> index; pid=254 -> panel hint if consistent, else -1).
  // We deliberately do NOT re-apply the panel hint here, so a stale hint can't
  // bypass the consistency check and mislabel an external run.
  // Only force-wake the display if the panel itself initiated this run.
  // External/scheduled runs let the normal sleep timeout apply.
  if (newly_entered && run_initiated_by_panel_) {
    view_.sleeping = false;
  }
  // Close the programs list screen when a run starts.
  view_.showing_programs_list = false;
  view_.showing_history = false;
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
  update_sync_view();
}

void PanelState::finish_idle_transition() {
  await_close_ = false;
  await_close_at_ms_ = 0;
  enter_idle();
  update_sync_view();
}

bool PanelState::pending_sync() const {
  return has_desired() || await_close_;
}

bool PanelState::sync_stale() const {
  if (!pending_sync()) return false;
  const uint32_t started_at = await_close_ ? await_close_at_ms_ : desired_at_ms_;
  return (now_ms_ - started_at) > kSyncTimeoutMs;
}

void PanelState::update_sync_view() {
  view_.show_syncing =
      refreshing_ || (pending_sync() && !sync_stale());
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

void PanelState::set_refreshing(bool refreshing) {
  refreshing_ = refreshing;
  update_sync_view();
}

void PanelState::set_controller_identity(const JoData& jo,
                                         const std::string& configured_host) {
  view_.controller_identity =
      jo.dname.empty() ? configured_host : jo.dname;
}

void PanelState::set_program_list(const JpData& jp) {
  jp_cache_ = jp;
}

void PanelState::open_programs_list() {
  on_touch(now_ms_);
  if (view_.phase == Phase::Idle) {
    view_.showing_history = false;
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

void PanelState::open_history() {
  on_touch(now_ms_);
  if (view_.phase == Phase::Idle) {
    view_.showing_programs_list = false;
    view_.showing_history = true;
    view_.hist_list_page = 0;
  }
}

void PanelState::close_history() {
  on_touch(now_ms_);
  view_.showing_history = false;
}

void PanelState::set_hist_list_page(int page, int total_pages) {
  on_touch(now_ms_);
  const int max_page = (total_pages > 0) ? (total_pages - 1) : 0;
  if (page < 0) page = 0;
  if (page > max_page) page = max_page;
  view_.hist_list_page = page;
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
  update_sync_view();
}

void PanelState::toggle_program_enabled_intent(int pid, bool en) {
  on_touch(now_ms_);
  desired_.kind = IntentKind::SetProgramEnabled;
  desired_.sid = pid;  // 0-based program index (as required by /cp?pid=)
  desired_.seconds = en ? 1 : 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
  update_sync_view();
}

void PanelState::pause_toggle_intent() {
  on_touch(now_ms_);
  desired_.kind = IntentKind::Pause;
  desired_.sid = -1;
  desired_.seconds = 0;
  desired_delivered_ = false;
  desired_at_ms_ = now_ms_;
  update_sync_view();
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
  update_sync_view();
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
  update_sync_view();

  // Countdown dead-reckoning for both manual and program running phases. Every
  // per-second timer the UI shows (station countdown, overall program time
  // remaining, and the pause "resumes in" clock) is advanced here from the
  // wall clock so they all tick smoothly between the 2 s /jc polls that
  // re-sync them.
  if (view_.phase == Phase::Running || view_.phase == Phase::ProgramRunning) {
    const uint32_t elapsed = now_ms - last_countdown_tick_ms_;
    const uint32_t secs = elapsed / 1000u;
    if (secs > 0u) {
      last_countdown_tick_ms_ += secs * 1000u;
      if (view_.paused) {
        // Paused: the station + overall countdowns freeze; only the resume
        // clock ticks down (smoothly, in step with the main timer).
        if (view_.pause_remaining_s > 0) {
          view_.pause_remaining_s =
              (secs >= static_cast<uint32_t>(view_.pause_remaining_s))
                  ? 0
                  : view_.pause_remaining_s - static_cast<int>(secs);
        }
      } else {
        if (view_.countdown_s > 0) {
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
        // Overall program time remaining ticks in step with the station timer.
        if (view_.phase == Phase::ProgramRunning &&
            view_.prog_run.total_remaining_seconds > 0) {
          view_.prog_run.total_remaining_seconds =
              (secs >= static_cast<uint32_t>(
                           view_.prog_run.total_remaining_seconds))
                  ? 0
                  : view_.prog_run.total_remaining_seconds -
                        static_cast<int>(secs);
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
  view_.enabled = (jc.en != 0);
  const int64_t rain_remaining =
      (jc.rd != 0) ? static_cast<int64_t>(jc.rdst) - jc.devt : 0;
  view_.rain_delay_seconds_remaining =
      rain_remaining <= 0
          ? 0
          : static_cast<int>(std::min<int64_t>(
                rain_remaining, std::numeric_limits<int>::max()));
  view_.current_ma = jc.curr;
  view_.has_current = jc.has_curr;
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
    // Resolve which saved program this run is, independent of what launched it:
    //   scheduled run  -> pid=program_index+1 (already set by the resolver)
    //   manual/app run -> pid=254 (program_index=-1); recover the index by
    //                     matching the live station set, then fall back to a
    //                     panel-launched hint if the match is ambiguous.
    int eff_idx = prog_state.program_index;
    if (eff_idx < 0) {
      // pid=254: a manual/app/panel program run with no program index in /jc.
      // We label the run *only* when the panel itself started it — the
      // remembered index is authoritative and we keep using it as long as it
      // stays consistent with the live station set (a stale hint from a just-
      // ended run can't leak into a new one). For any other pid=254 run (started
      // from the OpenSprinkler app or elsewhere) we do NOT guess which program
      // it is: leave program_index = -1 and render the live queue as reported.
      if (run_initiated_by_panel_ && launched_program_index_ >= 0 &&
          live_stations_in_program(jp_cache_, ps, 254, launched_program_index_)) {
        eff_idx = launched_program_index_;
      }
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
      if (jc.ps[sid].rem <= 0) continue;
      // Normally the running station's board bit is set. While paused the
      // controller turns the valve OFF (sbits clears) and pushes the queue's
      // start into the future, but the manual entry (pid=99) stays in ps[] with
      // a frozen rem. Detect it from ps[] so a paused manual run holds
      // Phase::Running instead of collapsing to idle.
      const bool on = board_bit_set(jc.sbits, sid);
      const bool paused_manual = (jc.pq != 0) && jc.ps[sid].pid == 99;
      if (on || paused_manual) {
        jc_running_sid = sid;
        jc_running_rem = jc.ps[sid].rem;
        break;
      }
    }
  }

  // #143: classify the live manual run using the durable panel-manual session
  // latch rather than the raw run_initiated_by_panel_ boolean. The run is ours
  // when the latch is active AND either the currently-on manual station is the
  // sid we launched, or a panel manual Run intent is still in flight (an
  // advance/reselect/auto-advance where the controller may momentarily still
  // report the previous station). Any manual run we can't account for is a
  // genuine external run and is rendered as the program "Station Queue" view.
  const bool manual_run = (prog_state.run_class == RunClass::ManualRun);
  const bool manual_intent_pending = (desired_.kind == IntentKind::Run);
  bool manual_is_ours = false;
  if (manual_run && panel_manual_active_) {
    manual_is_ours =
        (jc_running_sid == panel_manual_sid_) || manual_intent_pending;
  }
  if (manual_is_ours) {
    // Re-affirm the session: refresh the hysteresis anchor so the grace only
    // counts genuine idle time, and keep identity aligned with the station the
    // controller actually reports on (handles the advance handoff).
    panel_manual_seen_ms_ = now_ms_;
    if (jc_running_sid != -1) panel_manual_sid_ = jc_running_sid;
    run_initiated_by_panel_ = true;
    view_.run_initiated_by_panel = true;
  }
  const bool external_manual = manual_run && !manual_is_ours;
  ProgramRunState manual_q;
  if (external_manual) {
    // A pid=99 run the panel did not launch ends any (mismatched) session latch
    // and drops the panel-initiated flags so it renders/sleeps as external.
    end_panel_manual_session();
    run_initiated_by_panel_ = false;
    view_.run_initiated_by_panel = false;
    manual_q = resolve_manual_queue_state(ps, static_cast<long>(jc.devt));
  }

  if (prog_state.run_class == RunClass::ProgramRun) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_program_running(prog_state);
  } else if (external_manual && !manual_q.queue.empty()) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_program_running(manual_q);
  } else if (jc_running_sid != -1) {
    await_close_ = false;
    await_close_at_ms_ = 0;
    enter_running(jc_running_sid, jc_running_rem);
  } else {
    // A panel-launched run is confirmed asynchronously: after we issue the
    // command (single manual station via /cm, or a program via /mp), the first
    // /jc poll(s) can still report idle before the controller schedules the run.
    // While that launch intent is still pending we must NOT wipe the panel-
    // launched identity (run_initiated_by_panel_ / launched_program_index_):
    //  - for a program, dropping it makes the run come back as a generic
    //    "Program" with a live-shrinking queue instead of the launched program's
    //    name and full station set;
    //  - for a single manual station, dropping run_initiated_by_panel_ makes the
    //    confirming pid=99 poll look like an *external* manual run, which is
    //    rendered as the program "Station Queue" screen instead of the manual-
    //    station view (a hardware-only bug: the emulator reflects /cm instantly,
    //    so no intermediate idle poll ever cleared the flag).
    // The live_stations_in_program() consistency guard in the ProgramRun path
    // still prevents a stale hint from mislabelling a genuinely different run,
    // and sync_stale() bounds how long a never-confirmed manual launch keeps the
    // flag so a failed /cm can't hold the panel awake indefinitely.
    const bool program_launch_pending =
        (desired_.kind == IntentKind::RunProgram);
    const bool manual_launch_pending =
        (desired_.kind == IntentKind::Run) && !sync_stale();
    // #143: the panel-manual session is durable. This idle/non-manual poll only
    // ends it once the controller has been idle past the hysteresis grace (which
    // absorbs the auto-advance boundary gap, launch lag, a pause blip and a
    // single dropped poll). A still-pending manual launch (/cm not yet reflected)
    // always holds the session; sync_stale() bounds a never-confirmed launch so a
    // failed /cm self-heals. run_initiated_by_panel_ is cleared only when neither
    // a program launch is pending nor a manual session is latched.
    if (panel_manual_active_ && !manual_launch_pending &&
        (now_ms_ - panel_manual_seen_ms_) > kManualSessionGraceMs) {
      end_panel_manual_session();
    }
    if (!program_launch_pending && !panel_manual_active_) {
      // Idle: clear run_initiated flag so backlight/sleep behave normally.
      run_initiated_by_panel_ = false;
      view_.run_initiated_by_panel = false;
    }
    if (await_close_) {
      finish_idle_transition();
    } else if (desired_.kind == IntentKind::Stop) {
      // Stop confirmed idle: end the panel-manual session now so a later
      // external pid=99 run isn't claimed as ours during the grace window.
      end_panel_manual_session();
      run_initiated_by_panel_ = false;
      view_.run_initiated_by_panel = false;
      enter_idle();
      clear_desired();
      return;
    } else if (has_desired()) {
      if (program_launch_pending) {
        // Preserve the launched program index across enter_idle() so the next
        // poll that reports pid=254 can still label the run.
        const int saved_launched = launched_program_index_;
        enter_idle();
        launched_program_index_ = saved_launched;
      } else {
        enter_idle();
      }
    } else if (view_.phase == Phase::Running ||
               view_.phase == Phase::ProgramRunning) {
      finish_idle_transition();
    } else {
      enter_idle();
    }
  }

  reconcile_desired_after_jc();
  update_sync_view();
}

void PanelState::on_touch(uint32_t now_ms) {
  now_ms_ = now_ms;
  last_touch_ms_ = now_ms;
  view_.sleeping = false;
}

void PanelState::select_station(int sid) {
  on_touch(now_ms_);
  if (model_.runnable_index(sid) == -1) return;
  // Re-selecting the station that is already running intentionally RESTARTS it
  // with the currently selected run time (delivered as off-then-on via
  // extend()), so every selection queues a fresh Run — we no longer early-out
  // on same-sid. Editing the run time alone still does not requeue (see
  // set_run_time); a restart requires an explicit tap on the station.
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
