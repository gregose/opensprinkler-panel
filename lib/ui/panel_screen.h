// Full LVGL panel screen for the OpenSprinkler panel (lib/ui).
//
// This is the portable, LVGL-only composite that builds EVERY on-glass screen
// state onto a single active screen (idle prompt, unified run screen, settings
// panel, station grid, programs-list overlay, program-run queue, sleep overlay)
// plus the shared top bar. It mirrors the osp::ui::TopBar pattern from #124:
// build_* creates the widgets and returns handles; update_panel_screen() drives
// them per-frame from the pure view model (osp::PanelView + StationModel + the
// programs cache). No Arduino, no TFT_eSPI, no network, so the firmware and the
// host `sim` link the SAME code and render identical pixels.
//
// The firmware wires its LVGL display/touch drivers + poll loop to these; the
// sim drives them from fixtures. Event callbacks are injected by the caller as
// generic lv_event_cb_t (the firmware passes its handlers; the sim passes none)
// so this unit stays free of firmware symbols.
#pragma once

#include <lvgl.h>

#include "battery_monitor.h"  // osp::BatteryTier
#include "history_view.h"     // osp::ui::HistoryView, osp::ui::HistoryEntry
#include "os_client.h"        // osp::JpData
#include "panel_state.h"      // osp::PanelView
#include "station_model.h"    // osp::StationModel, osp::GridLayout
#include "top_bar.h"
#include "ui_layout.h"

namespace osp {
namespace ui {

// Firmware-supplied status that is NOT part of the pure PanelView: the panel's
// own Wi-Fi signal, the battery gauge sample, and the configured controller
// host string (used in the idle offline/auth error copy). The sim supplies
// fixture values.
struct HostStatus {
    int         panel_rssi_bars = 0;     // osp::rssi_to_bars(WiFi.RSSI())
    bool        panel_connected = false; // WiFi.status() == WL_CONNECTED
    int         battery_percent = 0;
    BatteryTier battery_tier    = BatteryTier::Healthy;
    const char* host_name       = "";    // configured OpenSprinkler host
};

// Event callbacks the caller attaches to the interactive widgets. All are
// plain LVGL callbacks so lib/ui carries no firmware dependencies; leave any
// null (the default) to build a non-interactive screen (e.g. the sim).
struct Callbacks {
    lv_event_cb_t on_screen_pressed = nullptr;  // any-touch (sleep wake)
    lv_event_cb_t on_pill           = nullptr;  // station pill tapped
    lv_event_cb_t on_next           = nullptr;
    lv_event_cb_t on_pause          = nullptr;
    lv_event_cb_t on_stop           = nullptr;
    lv_event_cb_t on_rt_minus       = nullptr;
    lv_event_cb_t on_rt_plus        = nullptr;
    lv_event_cb_t on_auto_adv       = nullptr;
    lv_event_cb_t on_open_programs  = nullptr;
    lv_event_cb_t on_close_programs = nullptr;
    lv_event_cb_t on_prog_page_prev = nullptr;
    lv_event_cb_t on_prog_page_next = nullptr;
    lv_event_cb_t on_prog_toggle    = nullptr;
    lv_event_cb_t on_prog_run       = nullptr;
    // #127 History overlay: entry button + Back + pager (mirrors programs).
    lv_event_cb_t on_open_history   = nullptr;
    lv_event_cb_t on_close_history  = nullptr;
    lv_event_cb_t on_hist_page_prev = nullptr;
    lv_event_cb_t on_hist_page_next = nullptr;
};

// All widget handles produced by build_panel_screen(). Grouped by screen region
// to document the decomposition; the caller keeps this to drive updates and to
// read station/program ids back from tapped widgets.
struct PanelScreen {
    lv_obj_t* scr = nullptr;

    // Top bar (built by osp::ui::build_top_bar).
    TopBar tb;

    // Idle prompt (left).
    lv_obj_t* pnl_idle      = nullptr;
    lv_obj_t* lbl_idle_head = nullptr;
    lv_obj_t* lbl_idle_sub  = nullptr;

    // Shared running-status panel (manual + program).
    lv_obj_t* pnl_status    = nullptr;
    lv_obj_t* lbl_eyebrow   = nullptr;
    lv_obj_t* lbl_stn_name  = nullptr;
    lv_obj_t* lbl_countdown = nullptr;
    lv_obj_t* lbl_paused    = nullptr;
    lv_obj_t* lbl_resume    = nullptr;

    // Shared action row (running only): Next / Pause / Stop.
    lv_obj_t* btn_next      = nullptr;
    lv_obj_t* btn_pause     = nullptr;
    lv_obj_t* lbl_pause_txt = nullptr;
    lv_obj_t* btn_stop      = nullptr;

    // Settings panel (right).
    lv_obj_t* pnl_right     = nullptr;
    lv_obj_t* btn_rt_minus  = nullptr;
    lv_obj_t* lbl_rt_value  = nullptr;
    lv_obj_t* btn_rt_plus   = nullptr;
    lv_obj_t* sw_auto_adv   = nullptr;
    lv_obj_t* btn_programs  = nullptr;
    lv_obj_t* btn_history   = nullptr;

    // Station grid (bottom). pill_count pills are live in stn_pills[].
    lv_obj_t* lbl_grid_title           = nullptr;
    lv_obj_t* grid_cont                = nullptr;
    lv_obj_t* stn_pills[24]            = {};
    lv_obj_t* stn_pill_lbls[24]        = {};
    int       pill_count               = 0;

    // Program-run queue list (right, program run only).
    lv_obj_t* pnl_prog_qlist  = nullptr;
    lv_obj_t* lbl_qlist_hdr   = nullptr;
    lv_obj_t* lbl_qlist_total = nullptr;
    lv_obj_t* qfade_top       = nullptr;
    lv_obj_t* qfade_bottom    = nullptr;
    lv_obj_t* qrow_mark[MAX_QROWS] = {};
    lv_obj_t* qrow_name[MAX_QROWS] = {};
    lv_obj_t* qrow_dur[MAX_QROWS]  = {};

    // Programs-list overlay (full width).
    lv_obj_t* pnl_programs                          = nullptr;
    lv_obj_t* prog_rows[MAX_PROG_ROWS]              = {};
    lv_obj_t* prog_row_icon[MAX_PROG_ROWS]          = {};
    lv_obj_t* prog_row_name[MAX_PROG_ROWS]          = {};
    lv_obj_t* prog_row_next[MAX_PROG_ROWS]          = {};
    lv_obj_t* prog_row_btn_toggle[MAX_PROG_ROWS]    = {};
    lv_obj_t* prog_row_btn_run[MAX_PROG_ROWS]       = {};
    lv_obj_t* prog_page_dots[MAX_PROG_PAGES]        = {};
    lv_obj_t* prog_page_prev                        = nullptr;
    lv_obj_t* prog_page_next                        = nullptr;

    // History overlay (#127) - full-width, compact single-line rows.
    lv_obj_t* pnl_history                           = nullptr;
    lv_obj_t* lbl_hist_empty                        = nullptr;
    lv_obj_t* hist_rows[MAX_HIST_ROWS]              = {};
    lv_obj_t* hist_row_icon[MAX_HIST_ROWS]          = {};
    lv_obj_t* hist_row_name[MAX_HIST_ROWS]          = {};
    lv_obj_t* hist_row_tag[MAX_HIST_ROWS]           = {};
    lv_obj_t* hist_row_dur[MAX_HIST_ROWS]           = {};
    lv_obj_t* hist_row_when[MAX_HIST_ROWS]          = {};
    lv_obj_t* hist_page_label                       = nullptr;
    lv_obj_t* hist_page_prev                        = nullptr;
    lv_obj_t* hist_page_next                        = nullptr;

    // Sleep overlay.
    lv_obj_t* sleep_overlay = nullptr;

    // Retained inputs for grid rebuilds + per-instance drop-pulse state.
    Callbacks cbs;
    bool drop_pulsing = false;
};

// Build the full panel screen onto `scr` (typically lv_screen_active()). The
// caller's event callbacks are attached to the interactive widgets. Panels
// start in the idle-visible configuration; call update_panel_screen() to drive
// live state and build_station_grid() to (re)populate the pills.
PanelScreen build_panel_screen(lv_obj_t* scr, const Callbacks& cbs);

// (Re)build the station pill grid from the model's runnable stations. Safe to
// call repeatedly (deletes existing pills first); reattaches the pill callback.
void build_station_grid(PanelScreen& ps, StationModel& model);

// Drive every widget from the current view model. `programs` is the /jp cache
// used by the programs list + program-run queue header; `history` is the
// render-only log span for the History overlay (#127). The backlight
// side-effect stays in the firmware caller.
void update_panel_screen(PanelScreen& ps,
                         const PanelView& v,
                         StationModel& model,
                         const JpData& programs,
                         const HistoryView& history,
                         const HostStatus& host);

}  // namespace ui
}  // namespace osp
