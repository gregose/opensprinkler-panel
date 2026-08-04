// Hardware-independent UI state machine for the OpenSprinkler station panel.
//
// Pure C++ (no Arduino, no LVGL, no network) so it builds and runs under the
// PlatformIO `native` test environment. It composes StationModel navigation
// with the M6 "single-intent / confirmed-only display" resilience model.
#pragma once

#include <cstdint>
#include <string>

#include "os_client.h"
#include "station_model.h"

namespace osp {

enum class Phase { Idle, Running, ProgramRunning };

enum class LinkState {
  Connected,
  Reconnecting,
  Offline,
  AuthError,
};

enum class TopBarState {
  Clean,
  Syncing,
  AuthError,
  Reconnecting,
  Offline,
  Disabled,
  RainDelay,
};

enum class IntentKind {
  None,
  Run,
  Stop,
  RunProgram,         // run a program now; pid stored in sid field
  SetProgramEnabled,  // enable/disable a program; pid in sid, bool in seconds
  Pause,              // toggle pause (10 min fixed)
  ProgramAdvance,     // skip current program station (ssta=1); current_sid in sid
};

struct DesiredIntent {
  IntentKind kind = IntentKind::None;
  int sid = -1;
  int seconds = 0;
};

struct PanelView {
  Phase phase = Phase::Idle;
  bool sleeping = false;
  bool station_list_loaded = false;
  int running_sid = -1;
  int countdown_s = 0;
  int run_time_s = 60;
  bool auto_advance = false;
  int ctrl_rssi = 0;
  LinkState link = LinkState::Connected;
  std::string controller_identity;
  bool enabled = true;
  int rain_delay_seconds_remaining = 0;
  int current_ma = 0;
  bool has_current = false;
  bool show_syncing = false;

  // M9: program screen navigation and run state
  bool showing_programs_list = false;
  int prog_list_page = 0;

  // #127: history (run/event log) screen navigation. UI-only; the entries are
  // supplied to the renderer separately (sim fixtures in PR1, /jl cache later).
  bool showing_history = false;
  int hist_list_page = 0;
  ProgramRunState prog_run;     // current program run queue (ProgramRunning phase)
  bool paused = false;          // controller is paused (/jc pq flag)
  int pause_remaining_s = 0;   // seconds left in pause (/jc pt)
  int sunrise_min = 0;          // minutes since midnight (from /jc)
  int sunset_min = 0;           // minutes since midnight (from /jc)
  long ctrl_devt = 0;           // controller local epoch (from /jc devt)
  bool run_initiated_by_panel = false;  // true when panel started this run
};

class PanelState {
 public:
  static constexpr int kDefaultRunTime = 60;
  static constexpr int kMinRunTime = 15;
  static constexpr int kMaxRunTime = 600;
  static constexpr int kRunTimeStep = 15;
  static constexpr uint32_t kDefaultSleepTimeoutMs = 300000;  // 5 minutes
  static constexpr uint32_t kSyncTimeoutMs = 20000;
  static constexpr int kConfirmGraceSeconds = 10;
  // #143: hysteresis window for the panel-manual session latch. A panel-launched
  // manual run must survive a transient idle/non-manual /jc poll (auto-advance
  // boundary gap, launch lag, pause blip, a single dropped poll) without being
  // reclassified as external. Picked comfortably above 2x the ~2 s poll interval
  // and well under the 15 s minimum run time.
  static constexpr uint32_t kManualSessionGraceMs = 5000;
  // #143: when auto-advancing a panel-manual run, an idle /jc poll only means the
  // station genuinely finished (as opposed to a transient mid-run idle blip) when
  // the dead-reckoned countdown is essentially spent. Used to drive advancing off
  // an observed finish when a dead-reckoned tick() did not fire the boundary first.
  static constexpr int kAutoAdvanceFinishSlackS = 3;

  explicit PanelState(StationModel& model,
                      int default_run_time_s = kDefaultRunTime);

  void tick(uint32_t now_ms);

  void on_jc(const JcData& jc, uint32_t now_ms);
  void on_link_connected(uint32_t now_ms);
  void on_link_reconnecting(uint32_t now_ms);
  void on_link_offline(uint32_t now_ms);
  void on_auth_error(uint32_t now_ms);
  void set_station_list_loaded(bool loaded);
  void set_refreshing(bool refreshing);
  void set_controller_identity(const JoData& jo,
                               const std::string& configured_host);

  // M9: programs list data (injected from network task after fetch_jp)
  void set_program_list(const JpData& jp);
  const JpData& program_list() const { return jp_cache_; }

  // M9: programs list navigation (UI-only, no network)
  void open_programs_list();
  void close_programs_list();
  void set_prog_list_page(int page);

  // #127: history screen navigation (UI-only, no network). Mirrors the programs
  // list: openable only from idle, paged, closed back to idle.
  void open_history();
  void close_history();
  void set_hist_list_page(int page, int total_pages);

  // M9: program actions (queue intents delivered by network task)
  void run_program_intent(int pid);
  void toggle_program_enabled_intent(int pid, bool en);
  void pause_toggle_intent();
  void program_advance_intent();  // skip current program station via ssta=1

  void select_station(int sid);
  void advance();
  void prev();
  void stop();
  void set_run_time(int t_sec);
  void set_auto_advance(bool enabled);
  void on_touch(uint32_t now_ms);

  // Idle-sleep timeout control. 0 disables sleep entirely (stays awake).
  // Configurable so the value can be tuned/persisted (NVS) and shortened for
  // on-device testing without waiting the full production timeout.
  void set_sleep_timeout_ms(uint32_t ms) { sleep_timeout_ms_ = ms; }
  uint32_t sleep_timeout_ms() const { return sleep_timeout_ms_; }
  // Milliseconds since the last touch/interaction (idle age). Useful for
  // heartbeat/telemetry so a bench can watch the idle timer climb.
  uint32_t idle_elapsed_ms() const {
    return (now_ms_ >= last_touch_ms_) ? now_ms_ - last_touch_ms_ : 0u;
  }

  const PanelView& view() const { return view_; }
  DesiredIntent desired() const { return desired_; }
  bool has_desired() const { return desired_.kind != IntentKind::None; }
  bool desired_delivered() const { return desired_delivered_; }
  bool awaiting_close() const { return await_close_; }
  bool pending_sync() const;
  bool sync_stale() const;
  bool can_deliver_desired() const;

  void mark_desired_delivered();
  void mark_desired_needs_retry();

 private:
  StationModel& model_;
  PanelView view_;
  DesiredIntent desired_;
  JpData jp_cache_;

  uint32_t now_ms_ = 0;
  uint32_t last_touch_ms_ = 0;
  uint32_t sleep_timeout_ms_ = kDefaultSleepTimeoutMs;
  uint32_t last_countdown_tick_ms_ = 0;
  uint32_t desired_at_ms_ = 0;
  uint32_t await_close_at_ms_ = 0;
  bool desired_delivered_ = false;
  bool await_close_ = false;
  bool refreshing_ = false;
  bool initialized_ = false;
  bool run_initiated_by_panel_ = false;
  // #143: durable panel-manual session latch. Manual stations report pid=99 in
  // /jc whether the panel or the OpenSprinkler app started them, so the panel's
  // own memory is the only way to tell "mine" from "external". Unlike the raw
  // run_initiated_by_panel_ boolean (which a single idle poll wipes), this latch
  // survives transient idle/non-manual polls via identity + hysteresis, so an
  // ongoing panel manual run can't be misread as external and stuck in
  // Phase::ProgramRunning.
  bool panel_manual_active_ = false;   // a panel-launched manual session is live
  int panel_manual_sid_ = -1;          // sid the panel launched (identity)
  uint32_t panel_manual_seen_ms_ = 0;  // last confirm/launch (hysteresis anchor)
  // 0-based index of a program the panel launched via /mp. Manual program runs
  // report pid=254 in /jc (program_index=-1), so we remember what we started to
  // label the running screen. Cleared on idle.
  int launched_program_index_ = -1;

  int clamp_run_time(int t_sec) const;
  void queue_desired_run(int sid);
  void queue_desired_stop();
  void clear_desired();
  void begin_panel_manual_session(int sid);
  void end_panel_manual_session();
  void enter_running(int sid, int countdown_s);
  void enter_program_running(const ProgramRunState& prog_state);
  void enter_idle();
  void begin_await_close();
  void finish_idle_transition();
  void reconcile_desired_after_jc();
  void update_sync_view();
  bool desired_matches_confirmed() const;
  bool should_allow_sleep() const;  // true when sleep timer is permitted to fire
};

std::string format_rain_delay(int seconds_remaining);
TopBarState resolve_top_bar_state(const PanelView& view);
std::string top_bar_status_text(const PanelView& view);

}  // namespace osp
