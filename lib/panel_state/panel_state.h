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

enum class IntentKind {
  None,
  Run,
  Stop,
  RunProgram,         // run a program now; pid stored in sid field
  SetProgramEnabled,  // enable/disable a program; pid in sid, bool in seconds
  Pause,              // toggle pause (10 min fixed)
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

  // M9: program screen navigation and run state
  bool showing_programs_list = false;
  int prog_list_page = 0;
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

  explicit PanelState(StationModel& model,
                      int default_run_time_s = kDefaultRunTime);

  void tick(uint32_t now_ms);

  void on_jc(const JcData& jc, uint32_t now_ms);
  void on_link_connected(uint32_t now_ms);
  void on_link_reconnecting(uint32_t now_ms);
  void on_link_offline(uint32_t now_ms);
  void on_auth_error(uint32_t now_ms);
  void set_station_list_loaded(bool loaded);

  // M9: programs list data (injected from network task after fetch_jp)
  void set_program_list(const JpData& jp);
  const JpData& program_list() const { return jp_cache_; }

  // M9: programs list navigation (UI-only, no network)
  void open_programs_list();
  void close_programs_list();
  void set_prog_list_page(int page);

  // M9: program actions (queue intents delivered by network task)
  void run_program_intent(int pid);
  void toggle_program_enabled_intent(int pid, bool en);
  void pause_toggle_intent();

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
  uint32_t idle_elapsed_ms() const { return now_ms_ - last_touch_ms_; }

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
  bool initialized_ = false;
  bool run_initiated_by_panel_ = false;

  int clamp_run_time(int t_sec) const;
  void queue_desired_run(int sid);
  void queue_desired_stop();
  void clear_desired();
  void enter_running(int sid, int countdown_s);
  void enter_program_running(const ProgramRunState& prog_state);
  void enter_idle();
  void begin_await_close();
  void finish_idle_transition();
  void reconcile_desired_after_jc();
  bool desired_matches_confirmed() const;
};

}  // namespace osp
