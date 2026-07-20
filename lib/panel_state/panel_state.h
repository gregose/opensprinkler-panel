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

enum class Phase { Idle, Running };

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
  std::string toast;
  LinkState link = LinkState::Connected;
};

class PanelState {
 public:
  static constexpr int kDefaultRunTime = 60;
  static constexpr int kMinRunTime = 15;
  static constexpr int kMaxRunTime = 600;
  static constexpr int kRunTimeStep = 15;
  static constexpr uint32_t kSleepTimeoutMs = 300000;
  static constexpr uint32_t kToastDurationMs = 3000;
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

  void select_station(int sid);
  void advance();
  void prev();
  void stop();
  void set_run_time(int t_sec);
  void set_auto_advance(bool enabled);
  void on_touch(uint32_t now_ms);

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
  enum class PendingFinish {
    None,
    Stopped,
    StationFinished,
    FinishedAllStations,
  };

  StationModel& model_;
  PanelView view_;
  DesiredIntent desired_;

  uint32_t now_ms_ = 0;
  uint32_t last_touch_ms_ = 0;
  uint32_t last_countdown_tick_ms_ = 0;
  uint32_t toast_set_ms_ = 0;
  uint32_t desired_at_ms_ = 0;
  uint32_t await_close_at_ms_ = 0;
  bool desired_delivered_ = false;
  bool await_close_ = false;
  bool initialized_ = false;
  PendingFinish pending_finish_ = PendingFinish::None;
  int pending_finish_sid_ = -1;

  int clamp_run_time(int t_sec) const;
  void post_toast(const std::string& msg);
  void queue_desired_run(int sid);
  void queue_desired_stop();
  void clear_desired();
  void enter_running(int sid, int countdown_s);
  void enter_idle();
  void begin_await_close(PendingFinish finish, int sid);
  void finish_idle_transition();
  void reconcile_desired_after_jc();
  bool desired_matches_confirmed() const;
  static int station_number(int sid) { return sid + 1; }
};

}  // namespace osp
