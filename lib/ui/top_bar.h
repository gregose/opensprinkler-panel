// Portable LVGL top bar for the OpenSprinkler panel (seed of lib/ui).
//
// The top bar is the first screen fragment factored out of src/main.cpp so it
// can be built and rendered on the host by the `sim` environment as well as on
// the ESP32 firmware. It is LVGL-only: no Arduino, no TFT_eSPI, no network. The
// firmware and the simulator link the SAME code here, so there is no HTML->LVGL
// translation drift.
#pragma once

#include <lvgl.h>

#include "battery_monitor.h"  // osp::BatteryTier

namespace osp {
namespace ui {

// "Current draw" slot (value + unit) shown on the right of the top bar.
struct CurrentSlot {
    lv_obj_t* box  = nullptr;
    lv_obj_t* val  = nullptr;
    lv_obj_t* unit = nullptr;
};

// RSSI meter widget: 4 ascending bar rectangles.
struct SigMeter {
    lv_obj_t* bars[4] = {};
};

// Battery gauge: pictogram (body + nub + fill) followed by a "NN%" label.
struct BattGlyph {
    lv_obj_t* body = nullptr;
    lv_obj_t* nub  = nullptr;
    lv_obj_t* fill = nullptr;
    lv_obj_t* pct  = nullptr;
};

// All handles produced by build_top_bar(); the caller keeps these to drive
// per-frame updates.
struct TopBar {
    lv_obj_t* bar        = nullptr;
    lv_obj_t* lbl_drop   = nullptr;
    lv_obj_t* lbl_name   = nullptr;
    lv_obj_t* lbl_status = nullptr;
    lv_obj_t* top_accent = nullptr;
    CurrentSlot current;
    SigMeter    sig_panel;
    SigMeter    sig_ctrl;
    BattGlyph   batt;
};

// Build the full top bar (bar container + left identity group + right cluster +
// 3px accent rule) onto `scr`. The current-draw slot starts hidden, matching the
// firmware's idle default; call update_* helpers to drive live state.
TopBar build_top_bar(lv_obj_t* scr);

// Update bar colours: first display_bars(quality, connected) bars filled,
// remainder dim; colour by quality tier.
void update_sig_meter(const SigMeter& m, int quality, bool connected);

// Update the battery fill width, tier colour and percent text.
void update_batt_glyph(const BattGlyph& g, int percent, osp::BatteryTier tier);

}  // namespace ui
}  // namespace osp
