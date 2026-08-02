// Portable LVGL panel-screen implementation (lib/ui). Extracted from
// src/main.cpp so the firmware and the host `sim` render identical pixels.
//
// build_panel_screen() == the old build_ui(); build_station_grid() == the old
// build_grid(); update_panel_screen() == the old ui_update(). The only changes
// from the firmware original are structural: widget handles live in a
// PanelScreen struct (not file-scope globals), event callbacks are injected via
// Callbacks, and the three firmware-only inputs (panel Wi-Fi signal, battery
// sample, controller host string) arrive via HostStatus. No behaviour changes.
#include "panel_screen.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "program_model.h"  // osp::program_run_eyebrow, osp::next_run
#include "widgets.h"        // make_btn, set_obj_id, get_obj_id
#include "ui_theme.h"       // CLR_*, hex_color, obj_set_hidden

// ui_font_countdown_48 is provided by the firmware (src/) and the sim
// (sim/ shim); declared here so lib/ui does not own the font asset.
extern "C" const lv_font_t ui_font_countdown_48;

namespace osp {
namespace ui {

namespace {

void fmt_countdown(char* buf, int secs) {
    if (secs < 0) secs = 0;
    snprintf(buf, 16, "%d:%02d", secs / 60, secs % 60);
}

// Drop-pulse animation exec callback (text opacity of the top-bar droplet).
void set_drop_text_opa(void* obj, int32_t value) {
    lv_obj_set_style_text_opa(static_cast<lv_obj_t*>(obj),
                              static_cast<lv_opa_t>(value), 0);
}

// Start/stop the syncing pulse on the top-bar droplet. Per-instance state
// lives in PanelScreen::drop_pulsing so multiple screens (e.g. the sim
// rendering several states) don't share a hidden static.
void update_drop_pulse(PanelScreen& ps, bool syncing) {
    if (syncing == ps.drop_pulsing) return;
    lv_obj_t* lbl_drop = ps.tb.lbl_drop;
    if (syncing) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, lbl_drop);
        lv_anim_set_exec_cb(&anim, set_drop_text_opa);
        lv_anim_set_values(&anim, LV_OPA_COVER, 90);
        lv_anim_set_duration(&anim, 600);
        lv_anim_set_playback_duration(&anim, 600);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    } else {
        lv_anim_delete(lbl_drop, set_drop_text_opa);
        lv_obj_set_style_text_opa(lbl_drop, LV_OPA_COVER, 0);
    }
    ps.drop_pulsing = syncing;
}

}  // namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------
PanelScreen build_panel_screen(lv_obj_t* scr, const Callbacks& cbs) {
    PanelScreen ps;
    ps.scr = scr;
    ps.cbs = cbs;

    lv_obj_set_style_bg_color(scr, hex_color(CLR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    if (cbs.on_screen_pressed) {
        lv_obj_add_event_cb(scr, cbs.on_screen_pressed, LV_EVENT_PRESSED, nullptr);
    }

    // ---- Top bar (built by lib/ui; same code as the host sim) ----------
    ps.tb = build_top_bar(scr);

    // ---- Left panel: Idle ---------------------------------------------
    ps.pnl_idle = lv_obj_create(scr);
    lv_obj_set_size(ps.pnl_idle, LEFT_W, PANEL_H);
    lv_obj_set_pos(ps.pnl_idle, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(ps.pnl_idle, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(ps.pnl_idle, 0, 0);
    lv_obj_set_style_pad_all(ps.pnl_idle, 14, 0);
    lv_obj_clear_flag(ps.pnl_idle, LV_OBJ_FLAG_SCROLLABLE);

    ps.lbl_idle_head = lv_label_create(ps.pnl_idle);
    lv_label_set_text(ps.lbl_idle_head, "Select a station");
    lv_obj_set_style_text_font(ps.lbl_idle_head, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ps.lbl_idle_head, hex_color(CLR_TEXT), 0);
    lv_obj_align(ps.lbl_idle_head, LV_ALIGN_TOP_LEFT, 0, 14);

    ps.lbl_idle_sub = lv_label_create(ps.pnl_idle);
    lv_label_set_text(ps.lbl_idle_sub, "Tap a station below to start");
    lv_obj_set_style_text_font(ps.lbl_idle_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_TEAL), 0);
    lv_label_set_long_mode(ps.lbl_idle_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ps.lbl_idle_sub, LEFT_W - 28);  // honour pad_all=14 each side
    lv_obj_align(ps.lbl_idle_sub, LV_ALIGN_TOP_LEFT, 0, 50);

    // NOTE: the "Programs" entry button lives in the right settings panel,
    // below Auto-advance (built further down), to match the mockup.

    // ---- Shared running status panel (manual + program) ----------------
    // ONE status stack used by both the manual station run and the program run.
    // Eyebrow / name / countdown fonts+positions are identical in both modes;
    // only their TEXT differs (set in update). The Option B paused block sits
    // to the right of the frozen countdown. Stops at the shared action row.
    ps.pnl_status = lv_obj_create(scr);
    lv_obj_set_size(ps.pnl_status, LEFT_W, PANEL_H);
    lv_obj_set_pos(ps.pnl_status, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(ps.pnl_status, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(ps.pnl_status, 0, 0);
    lv_obj_set_style_pad_all(ps.pnl_status, 14, 0);
    lv_obj_clear_flag(ps.pnl_status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ps.pnl_status, LV_OBJ_FLAG_HIDDEN);  // hidden until running

    ps.lbl_eyebrow = lv_label_create(ps.pnl_status);
    lv_label_set_text(ps.lbl_eyebrow, "STATION 1");
    lv_obj_set_style_text_font(ps.lbl_eyebrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.lbl_eyebrow, hex_color(CLR_TEAL), 0);
    lv_obj_align(ps.lbl_eyebrow, LV_ALIGN_TOP_LEFT, 0, 2);

    ps.lbl_stn_name = lv_label_create(ps.pnl_status);
    lv_label_set_text(ps.lbl_stn_name, "");
    lv_obj_set_width(ps.lbl_stn_name, LEFT_W - 28);
    // Constrain to a single line so LONG_DOT ellipsizes instead of wrapping -
    // otherwise a long name grows down into the countdown ticker at y=66.
    // montserrat_24 line height is ~29px; 30 keeps one line with a small margin.
    lv_obj_set_height(ps.lbl_stn_name, 30);
    lv_label_set_long_mode(ps.lbl_stn_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ps.lbl_stn_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ps.lbl_stn_name, hex_color(CLR_TEXT), 0);
    lv_obj_align(ps.lbl_stn_name, LV_ALIGN_TOP_LEFT, 0, 22);

    ps.lbl_countdown = lv_label_create(ps.pnl_status);
    lv_label_set_text(ps.lbl_countdown, "0:00");
    lv_obj_set_style_text_font(ps.lbl_countdown, &ui_font_countdown_48, 0);
    lv_obj_set_style_text_color(ps.lbl_countdown, hex_color(CLR_AMBER), 0);
    // Sit lower in the panel (near the action buttons) so it's clearly
    // separated from the station name above. Font line height is 32 px and the
    // panel content area is ~101 px, so y=66 leaves a comfortable gap under the
    // name without clipping the bottom (66 + 32 = 98 <= 101). Frozen while paused.
    lv_obj_align(ps.lbl_countdown, LV_ALIGN_TOP_LEFT, 0, 66);

    // Option B paused block: a two-line amber stack right-aligned to the panel
    // inner right edge, beside the frozen countdown. Right-aligning lets the gap
    // to the ~118px "10:00" countdown flex within the 262px inner width; line 2
    // is montserrat_12 so the widest string "Resumes in 10:00" clears the
    // countdown comfortably. Zero-padded MM:SS keeps the width stable across the
    // 9:59 -> 10:00 boundary. Both lines hidden unless paused (set in update).
    ps.lbl_paused = lv_label_create(ps.pnl_status);
    lv_label_set_text(ps.lbl_paused, "PAUSED");
    lv_obj_set_style_text_font(ps.lbl_paused, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ps.lbl_paused, hex_color(CLR_AMBER), 0);
    lv_obj_set_style_text_align(ps.lbl_paused, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(ps.lbl_paused, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ps.lbl_paused, LV_ALIGN_TOP_RIGHT, 0, 64);

    ps.lbl_resume = lv_label_create(ps.pnl_status);
    lv_label_set_text(ps.lbl_resume, "");
    lv_obj_set_style_text_font(ps.lbl_resume, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ps.lbl_resume, hex_color(CLR_AMBER), 0);
    lv_obj_set_style_text_align(ps.lbl_resume, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(ps.lbl_resume, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ps.lbl_resume, LV_ALIGN_TOP_RIGHT, 0, 84);

    // ---- Shared action row: Next / Pause / Stop ------------------------
    // ONE 3-up row at the shared ACTION_Y=156 used by both modes. Next
    // dispatches by mode (on_next); Pause and Stop are mode-agnostic.
    {
        static constexpr int ACTION_SIDE_PAD = 8;
        static constexpr int ACTION_BTN_GAP  = 8;
        const int action_btn_w =
            (LEFT_W - (2 * ACTION_SIDE_PAD) - (2 * ACTION_BTN_GAP)) / 3;

        ps.btn_next = make_btn(scr, "Next " LV_SYMBOL_RIGHT,
                               CLR_TEAL, CLR_BG, &lv_font_montserrat_16, 10);
        lv_obj_set_size(ps.btn_next, action_btn_w, ACTION_H);
        lv_obj_set_pos(ps.btn_next, ACTION_SIDE_PAD, ACTION_Y);
        lv_obj_add_flag(ps.btn_next, LV_OBJ_FLAG_HIDDEN);
        if (cbs.on_next)
            lv_obj_add_event_cb(ps.btn_next, cbs.on_next, LV_EVENT_CLICKED, nullptr);

        ps.btn_pause = make_btn(scr, "Pause",
                                CLR_LINE, CLR_TEXT, &lv_font_montserrat_16, 10);
        lv_obj_set_size(ps.btn_pause, action_btn_w, ACTION_H);
        lv_obj_set_pos(ps.btn_pause, ACTION_SIDE_PAD + action_btn_w + ACTION_BTN_GAP,
                       ACTION_Y);
        lv_obj_add_flag(ps.btn_pause, LV_OBJ_FLAG_HIDDEN);
        if (cbs.on_pause)
            lv_obj_add_event_cb(ps.btn_pause, cbs.on_pause, LV_EVENT_CLICKED, nullptr);
        ps.lbl_pause_txt = static_cast<lv_obj_t*>(lv_obj_get_child(ps.btn_pause, 0));

        ps.btn_stop = make_btn(scr, LV_SYMBOL_STOP " Stop",
                               CLR_RED, CLR_BG, &lv_font_montserrat_16, 10);
        lv_obj_set_size(ps.btn_stop, action_btn_w, ACTION_H);
        lv_obj_set_pos(ps.btn_stop, ACTION_SIDE_PAD + 2 * (action_btn_w + ACTION_BTN_GAP),
                       ACTION_Y);
        lv_obj_add_flag(ps.btn_stop, LV_OBJ_FLAG_HIDDEN);
        if (cbs.on_stop)
            lv_obj_add_event_cb(ps.btn_stop, cbs.on_stop, LV_EVENT_CLICKED, nullptr);
    }

    // ---- Right panel ---------------------------------------------------
    {
        ps.pnl_right = lv_obj_create(scr);
        lv_obj_t* pnl = ps.pnl_right;
        // The right column is an INVISIBLE panel (bg == CLR_BG, no border), so
        // its height is free to change with no visual box. Its old height
        // (CONTENT_H) put its bottom flush with the grid *container* top, but
        // the station pills actually begin ~18px lower (grid_cont sits at
        // GRID_Y+18), leaving an empty band on the right column. Extend the
        // panel down into that band, stopping NAV_BOT_GAP above the pills, so
        // the Programs + History nav buttons can be taller and reach closer to
        // the grid (the stepper + auto-advance above are unaffected).
        static constexpr int NAV_BOT_GAP   = 5;      // breathing gap above pills
        static constexpr int RIGHT_PANEL_H =
            (GRID_Y + 18 - NAV_BOT_GAP) - CONTENT_Y;  // taller than CONTENT_H
        lv_obj_set_size(pnl, RIGHT_W, RIGHT_PANEL_H);
        lv_obj_set_pos(pnl, LEFT_W, CONTENT_Y);
        lv_obj_set_style_bg_color(pnl, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(pnl, 0, 0);
        lv_obj_set_style_pad_all(pnl, 10, 0);
        lv_obj_clear_flag(pnl, LV_OBJ_FLAG_SCROLLABLE);

        // Vertical rhythm (#127 + bench polish): the right column stacks the
        // run-time stepper, auto-advance, then two nav buttons (Programs +
        // History). STEP_Y clears the "RUN TIME" label (montserrat_14, ~19px)
        // so the label is not crowded by the "-" button below it.
        static constexpr int STEP_Y = 24;
        static constexpr int STEP_H = 40;
        static constexpr int PANEL_PAD = 10;
        static constexpr int PANEL_CONTENT_W = RIGHT_W - (2 * PANEL_PAD);

        // Run time label — tucked just above the stepper (small gap) so it
        // reads clearly as that control's label rather than floating between
        // the panel top and the stepper.
        lv_obj_t* rt_lbl = lv_label_create(pnl);
        lv_label_set_text(rt_lbl, "RUN TIME");
        lv_obj_set_style_text_font(rt_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(rt_lbl, hex_color(CLR_MUTED), 0);
        lv_obj_align(rt_lbl, LV_ALIGN_TOP_LEFT, 0, 6);

        // Run-time stepper: [-] MM:SS [+]
        ps.btn_rt_minus = make_btn(pnl, LV_SYMBOL_MINUS,
                                   CLR_LINE, CLR_TEXT, &lv_font_montserrat_24, 9);
        lv_obj_set_size(ps.btn_rt_minus, 46, STEP_H);
        lv_obj_align(ps.btn_rt_minus, LV_ALIGN_TOP_LEFT, 0, STEP_Y);
        if (cbs.on_rt_minus)
            lv_obj_add_event_cb(ps.btn_rt_minus, cbs.on_rt_minus, LV_EVENT_CLICKED, nullptr);

        ps.lbl_rt_value = lv_label_create(pnl);
        lv_label_set_text(ps.lbl_rt_value, "1:00");
        lv_obj_set_style_text_font(ps.lbl_rt_value, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ps.lbl_rt_value, hex_color(CLR_TEXT), 0);
        lv_obj_align(ps.lbl_rt_value, LV_ALIGN_TOP_MID, 0,
                     STEP_Y + (STEP_H - lv_font_get_line_height(&lv_font_montserrat_20)) / 2);

        ps.btn_rt_plus = make_btn(pnl, LV_SYMBOL_PLUS,
                                  CLR_LINE, CLR_TEXT, &lv_font_montserrat_24, 9);
        lv_obj_set_size(ps.btn_rt_plus, 46, STEP_H);
        lv_obj_align(ps.btn_rt_plus, LV_ALIGN_TOP_RIGHT, 0, STEP_Y);
        if (cbs.on_rt_plus)
            lv_obj_add_event_cb(ps.btn_rt_plus, cbs.on_rt_plus, LV_EVENT_CLICKED, nullptr);

        // Auto-advance row sits directly under the stepper (no divider) so the
        // two run-time controls read as a group. ~8 px gap keeps touch targets
        // from colliding.
        static constexpr int AA_Y = STEP_Y + STEP_H + 6;  // just below stepper
        static constexpr int AA_H = 34;
        lv_obj_t* row_auto_adv = lv_obj_create(pnl);
        lv_obj_set_size(row_auto_adv, PANEL_CONTENT_W, AA_H);
        lv_obj_align(row_auto_adv, LV_ALIGN_TOP_LEFT, 0, AA_Y);
        lv_obj_set_style_bg_opa(row_auto_adv, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_auto_adv, 0, 0);
        lv_obj_set_style_pad_all(row_auto_adv, 0, 0);
        lv_obj_add_flag(row_auto_adv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row_auto_adv, LV_OBJ_FLAG_SCROLLABLE);
        if (cbs.on_auto_adv)
            lv_obj_add_event_cb(row_auto_adv, cbs.on_auto_adv, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl_aa_title = lv_label_create(row_auto_adv);
        lv_label_set_text(lbl_aa_title, "Auto-advance");
        lv_obj_set_style_text_font(lbl_aa_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_aa_title, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_aa_title, LV_ALIGN_LEFT_MID, 0, 0);

        ps.sw_auto_adv = lv_switch_create(row_auto_adv);
        lv_obj_align(ps.sw_auto_adv, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(ps.sw_auto_adv, hex_color(CLR_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(ps.sw_auto_adv, hex_color(CLR_TEAL),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_clear_flag(ps.sw_auto_adv, LV_OBJ_FLAG_CLICKABLE);

        // Programs + History entry buttons, stacked in the space below the
        // Auto-advance row. Both share the same width/style/height so the right
        // column reads as one family; plain text labels (no leading glyph) name
        // the screen each opens. Text uses CLR_TEXT like the other secondary
        // buttons rather than a teal accent.
        //
        // The panel now extends past the old grid-flush bottom (see
        // RIGHT_PANEL_H above) into the empty band above the station pills, so
        // the two buttons fill a taller nav band below auto-advance (8px
        // breathing gap below the toggle) down to NAV_BOT_GAP above the pills,
        // overlapping the 10px bottom pad (same bg) to reclaim it. This makes
        // each button noticeably taller (31->33px) and lets the pair reach
        // closer to the grid, while the stepper + auto-advance above keep their
        // legible spacing (with RUN TIME no longer crowding the stepper).
        static constexpr int NAV_BTN2_H  = 33;  // shared nav-button height
        static constexpr int NAV_BTN2_G  = 6;   // gap between the two buttons
        static constexpr int NAV_TOP_GAP = 8;   // gap below auto-advance toggle
        const int nav_top    = AA_Y + AA_H + NAV_TOP_GAP;  // below auto-adv
        const int nav_bottom = RIGHT_PANEL_H - PANEL_PAD;  // overlap bottom pad
        const int nav_space  = nav_bottom - nav_top;       // usable band
        const int nav_block  = 2 * NAV_BTN2_H + NAV_BTN2_G;  // both + gap
        int prog_btn_y = nav_top + (nav_space - nav_block) / 2;
        if (prog_btn_y < nav_top) prog_btn_y = nav_top;
        const int hist_btn_y = prog_btn_y + NAV_BTN2_H + NAV_BTN2_G;

        ps.btn_programs = make_btn(pnl, "Programs",
                                   CLR_LINE, CLR_TEXT, &lv_font_montserrat_16, 8);
        lv_obj_set_size(ps.btn_programs, PANEL_CONTENT_W, NAV_BTN2_H);
        lv_obj_align(ps.btn_programs, LV_ALIGN_TOP_LEFT, 0, prog_btn_y);
        if (cbs.on_open_programs)
            lv_obj_add_event_cb(ps.btn_programs, cbs.on_open_programs, LV_EVENT_CLICKED, nullptr);

        ps.btn_history = make_btn(pnl, "History",
                                  CLR_LINE, CLR_TEXT, &lv_font_montserrat_16, 8);
        lv_obj_set_size(ps.btn_history, PANEL_CONTENT_W, NAV_BTN2_H);
        lv_obj_align(ps.btn_history, LV_ALIGN_TOP_LEFT, 0, hist_btn_y);
        if (cbs.on_open_history)
            lv_obj_add_event_cb(ps.btn_history, cbs.on_open_history, LV_EVENT_CLICKED, nullptr);
    }

    // ---- Right-side program QUEUE list (M9) ----------------------------
    // Shown during a program run in place of the settings panel (full height,
    // since the grid is hidden). Lists the program's stations (done / current /
    // upcoming) reconstructed from the program definition + live /jc ps[].
    {
        ps.pnl_prog_qlist = lv_obj_create(scr);
        lv_obj_set_size(ps.pnl_prog_qlist, RIGHT_W, FULL_RIGHT_H);
        lv_obj_set_pos(ps.pnl_prog_qlist, LEFT_W, CONTENT_Y);
        lv_obj_set_style_bg_color(ps.pnl_prog_qlist, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(ps.pnl_prog_qlist, 0, 0);
        lv_obj_set_style_pad_all(ps.pnl_prog_qlist, 10, 0);
        lv_obj_clear_flag(ps.pnl_prog_qlist, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ps.pnl_prog_qlist, LV_OBJ_FLAG_HIDDEN);

        const int qcontent_w = RIGHT_W - 20;

        ps.lbl_qlist_hdr = lv_label_create(ps.pnl_prog_qlist);
        lv_label_set_text(ps.lbl_qlist_hdr, "");
        lv_obj_set_width(ps.lbl_qlist_hdr, qcontent_w);
        // One-line height (montserrat_16 ~19px) so a long program name
        // ellipsizes instead of wrapping into the total-time line at y=22.
        lv_obj_set_height(ps.lbl_qlist_hdr, 20);
        lv_label_set_long_mode(ps.lbl_qlist_hdr, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.lbl_qlist_hdr, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ps.lbl_qlist_hdr, hex_color(CLR_TEXT), 0);
        lv_obj_align(ps.lbl_qlist_hdr, LV_ALIGN_TOP_LEFT, 0, 0);

        ps.lbl_qlist_total = lv_label_create(ps.pnl_prog_qlist);
        lv_label_set_text(ps.lbl_qlist_total, "");
        lv_obj_set_width(ps.lbl_qlist_total, qcontent_w);
        lv_label_set_long_mode(ps.lbl_qlist_total, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.lbl_qlist_total, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ps.lbl_qlist_total, hex_color(CLR_MUTED), 0);
        lv_obj_align(ps.lbl_qlist_total, LV_ALIGN_TOP_LEFT, 0, 22);

        static constexpr int QROW_Y0 = 50;
        static constexpr int QROW_H  = 24;

        for (int i = 0; i < MAX_QROWS; ++i) {
            const int y = QROW_Y0 + i * QROW_H;

            ps.qrow_mark[i] = lv_label_create(ps.pnl_prog_qlist);
            lv_label_set_text(ps.qrow_mark[i], "");
            lv_obj_set_style_text_font(ps.qrow_mark[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(ps.qrow_mark[i], hex_color(CLR_MUTED), 0);
            lv_obj_set_pos(ps.qrow_mark[i], 0, y);

            ps.qrow_name[i] = lv_label_create(ps.pnl_prog_qlist);
            lv_label_set_text(ps.qrow_name[i], "");
            lv_obj_set_width(ps.qrow_name[i], qcontent_w - 22 - 48);
            // Constrain to a single line so LONG_DOT truncates with an ellipsis
            // instead of wrapping (a long station name would otherwise grow
            // vertically and collide into the next queue row).
            lv_obj_set_height(ps.qrow_name[i], 18);
            lv_label_set_long_mode(ps.qrow_name[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(ps.qrow_name[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(ps.qrow_name[i], hex_color(CLR_TEXT), 0);
            lv_obj_set_pos(ps.qrow_name[i], 22, y);

            ps.qrow_dur[i] = lv_label_create(ps.pnl_prog_qlist);
            lv_label_set_text(ps.qrow_dur[i], "");
            lv_obj_set_width(ps.qrow_dur[i], 44);
            lv_obj_set_style_text_align(ps.qrow_dur[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(ps.qrow_dur[i], &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(ps.qrow_dur[i], hex_color(CLR_MUTED), 0);
            lv_obj_align(ps.qrow_dur[i], LV_ALIGN_TOP_RIGHT, 0, y);
        }

        // Fade masks (created last so they render on top of the rows). A short
        // vertical gradient of the panel background, opaque at the list edge
        // fading to transparent, signals there are more rows above/below - the
        // LVGL equivalent of the mockup's CSS mask. Toggled per frame.
        const int qfade_h = 16;
        ps.qfade_top = lv_obj_create(ps.pnl_prog_qlist);
        lv_obj_remove_style_all(ps.qfade_top);
        lv_obj_set_size(ps.qfade_top, qcontent_w, qfade_h);
        lv_obj_set_pos(ps.qfade_top, 0, QROW_Y0 - 3);
        lv_obj_set_style_bg_color(ps.qfade_top, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_color(ps.qfade_top, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_dir(ps.qfade_top, LV_GRAD_DIR_VER, 0);
        // Base fill must be opaque; the per-stop main/grad opacities below create
        // the actual fade. Without this, remove_style_all leaves bg_opa=0 and the
        // gradient never renders (the "no fade mask" bug).
        lv_obj_set_style_bg_opa(ps.qfade_top, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(ps.qfade_top, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_opa(ps.qfade_top, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ps.qfade_top, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ps.qfade_top, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ps.qfade_top, LV_OBJ_FLAG_HIDDEN);

        ps.qfade_bottom = lv_obj_create(ps.pnl_prog_qlist);
        lv_obj_remove_style_all(ps.qfade_bottom);
        lv_obj_set_size(ps.qfade_bottom, qcontent_w, qfade_h);
        lv_obj_set_pos(ps.qfade_bottom, 0, QROW_Y0 + MAX_QROWS * QROW_H - qfade_h - 2);
        lv_obj_set_style_bg_color(ps.qfade_bottom, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_color(ps.qfade_bottom, hex_color(CLR_BG), 0);
        lv_obj_set_style_bg_grad_dir(ps.qfade_bottom, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ps.qfade_bottom, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_main_opa(ps.qfade_bottom, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(ps.qfade_bottom, LV_OPA_COVER, 0);
        lv_obj_clear_flag(ps.qfade_bottom, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ps.qfade_bottom, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ps.qfade_bottom, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Grid area -----------------------------------------------------
    ps.lbl_grid_title = lv_label_create(scr);
    lv_label_set_text(ps.lbl_grid_title, "STATIONS");
    lv_obj_set_style_text_font(ps.lbl_grid_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ps.lbl_grid_title, hex_color(CLR_MUTED), 0);
    lv_obj_set_pos(ps.lbl_grid_title, 8, GRID_Y + 2);

    ps.grid_cont = lv_obj_create(scr);
    lv_obj_set_size(ps.grid_cont, SCREEN_W - 8, GRID_H - 20);
    lv_obj_set_pos(ps.grid_cont, 4, GRID_Y + 18);
    lv_obj_set_style_bg_color(ps.grid_cont, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(ps.grid_cont, 0, 0);
    lv_obj_set_style_pad_all(ps.grid_cont, 2, 0);
    lv_obj_set_style_pad_gap(ps.grid_cont, 6, 0);
    lv_obj_clear_flag(ps.grid_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(ps.grid_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ps.grid_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(ps.grid_cont,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // ---- Sleep overlay -------------------------------------------------
    ps.sleep_overlay = lv_obj_create(scr);
    lv_obj_set_size(ps.sleep_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(ps.sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(ps.sleep_overlay, hex_color(0x000000), 0);
    lv_obj_set_style_bg_opa(ps.sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(ps.sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ps.sleep_overlay, LV_OBJ_FLAG_EVENT_BUBBLE);

    // ---- Programs list panel (M9) - full-width overlay -----------------
    // The station grid is hidden here, so the panel uses the full height.
    //   Header: PROG_HDR_H px (larger Back target)
    //   4 rows:  PROG_ROW_H px each
    //   Pager:   below the rows
    static constexpr int PROG_HDR_H  = 44;
    static constexpr int PROG_ROW_H  = 48;
    // Pager sits in the empty band below the last row, vertically centred
    // between the bottom of the last program row and the bottom of the panel.
    static constexpr int PROG_ROWS_BOTTOM = PROG_HDR_H + MAX_PROG_ROWS * PROG_ROW_H;
    static constexpr int PROG_PAGER_CY = PROG_ROWS_BOTTOM +
                                         (PROG_LIST_H - PROG_ROWS_BOTTOM) / 2;
    static constexpr int PROG_DOT_Y  = PROG_PAGER_CY - PROG_DOT_H / 2;
    // Right-side button metrics within each row.
    static constexpr int PROG_RUN_W  = 80;   // Run button width
    static constexpr int PROG_TOG_W  = 86;   // Toggle (Enable/Disable) button width
    static constexpr int PROG_BTN_RP = 4;    // right padding from row edge
    static constexpr int PROG_BTN_G  = 4;    // gap between toggle and run
    static constexpr int PROG_BTN_H  = PROG_ROW_H - 12;
    static constexpr int PROG_BTN_Y  = (PROG_ROW_H - PROG_BTN_H) / 2;
    // x position of toggle and run buttons (within row, row width = SCREEN_W)
    static constexpr int PROG_RUN_X  = SCREEN_W - PROG_BTN_RP - PROG_RUN_W;
    static constexpr int PROG_TOG_X  = PROG_RUN_X - PROG_BTN_G - PROG_TOG_W;
    // Left content area: enable/disable icon + name / next-run label width
    static constexpr int PROG_ICON_W = 22;  // enable/disable glyph column
    static constexpr int PROG_TEXT_X = 8 + PROG_ICON_W;
    static constexpr int PROG_NAME_W = PROG_TOG_X - PROG_TEXT_X - 8;  // gap=8

    ps.pnl_programs = lv_obj_create(scr);
    lv_obj_set_size(ps.pnl_programs, SCREEN_W, PROG_LIST_H);
    lv_obj_set_pos(ps.pnl_programs, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(ps.pnl_programs, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(ps.pnl_programs, 0, 0);
    lv_obj_set_style_pad_all(ps.pnl_programs, 0, 0);
    lv_obj_clear_flag(ps.pnl_programs, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ps.pnl_programs, LV_OBJ_FLAG_HIDDEN);

    // Header row
    {
        lv_obj_t* hdr = lv_obj_create(ps.pnl_programs);
        lv_obj_set_size(hdr, SCREEN_W, PROG_HDR_H);
        lv_obj_set_pos(hdr, 0, 0);
        lv_obj_set_style_bg_color(hdr, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl_title = lv_label_create(hdr);
        lv_label_set_text(lbl_title, "PROGRAMS");
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_title, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t* btn_back = make_btn(hdr, LV_SYMBOL_LEFT " Back",
                                      CLR_LINE, CLR_TEXT,
                                      &lv_font_montserrat_20, 8);
        lv_obj_set_size(btn_back, 128, NAV_BTN_H);
        lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -8, 0);
        if (cbs.on_close_programs)
            lv_obj_add_event_cb(btn_back, cbs.on_close_programs, LV_EVENT_CLICKED, nullptr);
    }

    // 4 program rows
    for (int r = 0; r < MAX_PROG_ROWS; ++r) {
        const int ry = PROG_HDR_H + r * PROG_ROW_H;

        ps.prog_rows[r] = lv_obj_create(ps.pnl_programs);
        lv_obj_set_size(ps.prog_rows[r], SCREEN_W, PROG_ROW_H);
        lv_obj_set_pos(ps.prog_rows[r], 0, ry);
        lv_obj_set_style_bg_color(ps.prog_rows[r], hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(ps.prog_rows[r], 0, 0);
        lv_obj_set_style_pad_all(ps.prog_rows[r], 0, 0);
        lv_obj_clear_flag(ps.prog_rows[r], LV_OBJ_FLAG_SCROLLABLE);

        // Enable/disable icon on the name line (left). Glyph + colour convey
        // program state (paired with dimming of the name); no chip/word.
        ps.prog_row_icon[r] = lv_label_create(ps.prog_rows[r]);
        lv_label_set_text(ps.prog_row_icon[r], "");
        lv_obj_set_style_text_font(ps.prog_row_icon[r], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ps.prog_row_icon[r], hex_color(CLR_TEAL), 0);
        lv_obj_set_pos(ps.prog_row_icon[r], 8, 8);

        // Program name (top line). Colour conveys enabled state (dimmed when
        // disabled) - no separate ENABLED/DISABLED chip.
        ps.prog_row_name[r] = lv_label_create(ps.prog_rows[r]);
        lv_label_set_text(ps.prog_row_name[r], "");
        lv_obj_set_width(ps.prog_row_name[r], PROG_NAME_W);
        // Constrain to a single line so LONG_DOT ellipsizes instead of wrapping
        // - otherwise a long program name grows down into the schedule/meta line
        // at y=28. montserrat_16 line height is ~19px; 20 keeps one line.
        lv_obj_set_height(ps.prog_row_name[r], 20);
        lv_label_set_long_mode(ps.prog_row_name[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.prog_row_name[r], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(ps.prog_row_name[r], hex_color(CLR_TEXT), 0);
        lv_obj_set_pos(ps.prog_row_name[r], PROG_TEXT_X, 6);

        // Next-run + zones/runtime meta (bottom line, full width up to buttons).
        ps.prog_row_next[r] = lv_label_create(ps.prog_rows[r]);
        lv_label_set_text(ps.prog_row_next[r], "");
        lv_obj_set_width(ps.prog_row_next[r], PROG_NAME_W);
        lv_label_set_long_mode(ps.prog_row_next[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.prog_row_next[r], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ps.prog_row_next[r], hex_color(CLR_MUTED), 0);
        lv_obj_set_pos(ps.prog_row_next[r], PROG_TEXT_X, 28);

        // Toggle (Enable/Disable) button
        ps.prog_row_btn_toggle[r] = make_btn(ps.prog_rows[r], "Disable",
                                             CLR_LINE, CLR_TEXT,
                                             &lv_font_montserrat_12, 6);
        lv_obj_set_size(ps.prog_row_btn_toggle[r], PROG_TOG_W, PROG_BTN_H);
        lv_obj_set_pos(ps.prog_row_btn_toggle[r], PROG_TOG_X, PROG_BTN_Y);
        if (cbs.on_prog_toggle)
            lv_obj_add_event_cb(ps.prog_row_btn_toggle[r], cbs.on_prog_toggle,
                                LV_EVENT_CLICKED, nullptr);

        // Run button
        ps.prog_row_btn_run[r] = make_btn(ps.prog_rows[r], "Run " LV_SYMBOL_RIGHT,
                                          CLR_TEAL, CLR_BG,
                                          &lv_font_montserrat_12, 6);
        lv_obj_set_size(ps.prog_row_btn_run[r], PROG_RUN_W, PROG_BTN_H);
        lv_obj_set_pos(ps.prog_row_btn_run[r], PROG_RUN_X, PROG_BTN_Y);
        if (cbs.on_prog_run)
            lv_obj_add_event_cb(ps.prog_row_btn_run[r], cbs.on_prog_run,
                                LV_EVENT_CLICKED, nullptr);
    }

    // Pager: arrow buttons flanking a centred row of page dots (no text).
    {
        // Dots are non-interactive indicators; the arrows drive paging. Their X
        // is re-centred every frame in update from the live page count, so the
        // build-time X here is just a placeholder - only size/style/Y are set
        // now (dots stay hidden until there is more than one page).
        for (int d = 0; d < MAX_PROG_PAGES; ++d) {
            ps.prog_page_dots[d] = lv_obj_create(ps.pnl_programs);
            lv_obj_remove_style_all(ps.prog_page_dots[d]);
            lv_obj_set_size(ps.prog_page_dots[d], PROG_DOT_W, PROG_DOT_H);
            lv_obj_set_pos(ps.prog_page_dots[d], 0, PROG_DOT_Y);
            lv_obj_set_style_radius(ps.prog_page_dots[d], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(ps.prog_page_dots[d], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(ps.prog_page_dots[d], hex_color(CLR_LINE), 0);
            lv_obj_add_flag(ps.prog_page_dots[d], LV_OBJ_FLAG_HIDDEN);
        }

        // Touch-sized arrow buttons: same height as the Back button (NAV_BTN_H)
        // for nav-button continuity, vertically centred on the pager band.
        static constexpr int ARROW_INSET = 14;
        const int arrow_y = PROG_PAGER_CY - NAV_BTN_H / 2;

        ps.prog_page_prev = make_btn(ps.pnl_programs, LV_SYMBOL_LEFT,
                                     CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(ps.prog_page_prev, PROG_ARROW_W, NAV_BTN_H);
        lv_obj_set_pos(ps.prog_page_prev, ARROW_INSET, arrow_y);
        if (cbs.on_prog_page_prev)
            lv_obj_add_event_cb(ps.prog_page_prev, cbs.on_prog_page_prev,
                                LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(ps.prog_page_prev, LV_OBJ_FLAG_HIDDEN);

        ps.prog_page_next = make_btn(ps.pnl_programs, LV_SYMBOL_RIGHT,
                                     CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(ps.prog_page_next, PROG_ARROW_W, NAV_BTN_H);
        lv_obj_set_pos(ps.prog_page_next, SCREEN_W - PROG_ARROW_W - ARROW_INSET, arrow_y);
        if (cbs.on_prog_page_next)
            lv_obj_add_event_cb(ps.prog_page_next, cbs.on_prog_page_next,
                                LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(ps.prog_page_next, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- History list panel (#127) - full-width overlay ----------------
    // Mirrors the programs overlay (same header/Back) but with COMPACT
    // single-line rows so ~10 fit per page instead of 4. Each row is:
    //   [icon]  name  [program tag] .....(flex)..... duration  short-timestamp
    // No day grouping, no touch scrolling. The pager is a numeric "Page N / M"
    // label between prev/next arrows (not dots) so it scales to the many pages a
    // busy 30-day log produces.
    static constexpr int HIST_HDR_H   = 44;
    static constexpr int HIST_ROW_H   = 21;
    static constexpr int HIST_ARROW_H = 30;  // slim pager arrow (vs NAV_BTN_H 36)
    static constexpr int HIST_ROWS_BOTTOM = HIST_HDR_H + MAX_HIST_ROWS * HIST_ROW_H;
    static constexpr int HIST_PAGER_CY = HIST_ROWS_BOTTOM +
                                         (PROG_LIST_H - HIST_ROWS_BOTTOM) / 2;

    ps.pnl_history = lv_obj_create(scr);
    lv_obj_set_size(ps.pnl_history, SCREEN_W, PROG_LIST_H);
    lv_obj_set_pos(ps.pnl_history, 0, CONTENT_Y);
    lv_obj_set_style_bg_color(ps.pnl_history, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(ps.pnl_history, 0, 0);
    lv_obj_set_style_pad_all(ps.pnl_history, 0, 0);
    lv_obj_clear_flag(ps.pnl_history, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ps.pnl_history, LV_OBJ_FLAG_HIDDEN);

    // Header row: "HISTORY" + Back (same treatment as the programs list).
    {
        lv_obj_t* hdr = lv_obj_create(ps.pnl_history);
        lv_obj_set_size(hdr, SCREEN_W, HIST_HDR_H);
        lv_obj_set_pos(hdr, 0, 0);
        lv_obj_set_style_bg_color(hdr, hex_color(CLR_BG), 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl_title = lv_label_create(hdr);
        lv_label_set_text(lbl_title, "HISTORY");
        lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl_title, hex_color(CLR_TEXT), 0);
        lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 10, 0);

        // Retention annotation: the controller's run log only spans a fixed
        // recent window, so name it next to the title (muted, smaller) to set
        // expectations. Static "Last 30 days" for now; PR2 sources the real
        // window from the /jl range once live data is wired in.
        lv_obj_t* lbl_span = lv_label_create(hdr);
        lv_label_set_text(lbl_span, "Last 30 days");
        lv_obj_set_style_text_font(lbl_span, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl_span, hex_color(CLR_MUTED), 0);
        lv_obj_align_to(lbl_span, lbl_title, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -2);

        lv_obj_t* btn_back = make_btn(hdr, LV_SYMBOL_LEFT " Back",
                                      CLR_LINE, CLR_TEXT,
                                      &lv_font_montserrat_20, 8);
        lv_obj_set_size(btn_back, 128, NAV_BTN_H);
        lv_obj_align(btn_back, LV_ALIGN_RIGHT_MID, -8, 0);
        // Resistive touch near the screen edge is imprecise; enlarge the hit
        // zone well past the visual button so brief/edge taps still register.
        lv_obj_set_ext_click_area(btn_back, 16);
        if (cbs.on_close_history)
            lv_obj_add_event_cb(btn_back, cbs.on_close_history, LV_EVENT_CLICKED, nullptr);
    }

    // Empty-state message (centred), shown when there is no history yet.
    ps.lbl_hist_empty = lv_label_create(ps.pnl_history);
    lv_label_set_text(ps.lbl_hist_empty, "No history yet");
    lv_obj_set_style_text_font(ps.lbl_hist_empty, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ps.lbl_hist_empty, hex_color(CLR_MUTED), 0);
    lv_obj_align(ps.lbl_hist_empty, LV_ALIGN_CENTER, 0, HIST_HDR_H / 2);
    lv_obj_add_flag(ps.lbl_hist_empty, LV_OBJ_FLAG_HIDDEN);

    // Compact rows.
    for (int r = 0; r < MAX_HIST_ROWS; ++r) {
        const int ry = HIST_HDR_H + r * HIST_ROW_H;

        ps.hist_rows[r] = lv_obj_create(ps.pnl_history);
        lv_obj_set_size(ps.hist_rows[r], SCREEN_W, HIST_ROW_H);
        lv_obj_set_pos(ps.hist_rows[r], 0, ry);
        // Zebra stripe by row slot (stable regardless of content) so a dense log
        // is easier to scan across the name/duration/timestamp columns.
        lv_obj_set_style_bg_color(ps.hist_rows[r],
                                  hex_color((r & 1) ? CLR_ROW_ALT : CLR_BG), 0);
        lv_obj_set_style_border_width(ps.hist_rows[r], 0, 0);
        lv_obj_set_style_pad_all(ps.hist_rows[r], 0, 0);
        lv_obj_clear_flag(ps.hist_rows[r], LV_OBJ_FLAG_SCROLLABLE);

        // Station/program name (single line, ellipsized).
        ps.hist_row_name[r] = lv_label_create(ps.hist_rows[r]);
        lv_label_set_text(ps.hist_row_name[r], "");
        lv_obj_set_width(ps.hist_row_name[r], HIST_NAME_W);
        // Constrain to one line so LONG_DOT ellipsizes instead of wrapping when
        // the name is narrowed (e.g. rows that reserve room for the program tag).
        lv_obj_set_height(ps.hist_row_name[r],
                          lv_font_get_line_height(&lv_font_montserrat_14));
        lv_label_set_long_mode(ps.hist_row_name[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.hist_row_name[r], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ps.hist_row_name[r], hex_color(CLR_TEXT), 0);
        lv_obj_align(ps.hist_row_name[r], LV_ALIGN_LEFT_MID, HIST_NAME_X, 0);

        // Trailing context tag: the program name for a program run (or "Manual"
        // for a manual run), muted and smaller, in a fixed column just past the
        // narrowed name so it never collides with the duration zone. Fixed width
        // + LONG_DOT so a long program name ellipsizes inside its own column.
        ps.hist_row_tag[r] = lv_label_create(ps.hist_rows[r]);
        lv_label_set_text(ps.hist_row_tag[r], "");
        lv_obj_set_width(ps.hist_row_tag[r], HIST_TAG_W);
        lv_obj_set_height(ps.hist_row_tag[r],
                          lv_font_get_line_height(&lv_font_montserrat_12));
        lv_label_set_long_mode(ps.hist_row_tag[r], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(ps.hist_row_tag[r], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(ps.hist_row_tag[r], hex_color(CLR_MUTED), 0);
        lv_obj_align(ps.hist_row_tag[r], LV_ALIGN_LEFT_MID, HIST_TAG_X, 0);
        lv_obj_add_flag(ps.hist_row_tag[r], LV_OBJ_FLAG_HIDDEN);

        // Duration (right-aligned column).
        ps.hist_row_dur[r] = lv_label_create(ps.hist_rows[r]);
        lv_label_set_text(ps.hist_row_dur[r], "");
        lv_obj_set_width(ps.hist_row_dur[r], HIST_DUR_W);
        lv_obj_set_style_text_align(ps.hist_row_dur[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(ps.hist_row_dur[r], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ps.hist_row_dur[r], hex_color(CLR_LEDE), 0);
        lv_obj_align(ps.hist_row_dur[r], LV_ALIGN_RIGHT_MID,
                     -(HIST_RP + HIST_WHEN_W + HIST_COL_G), 0);

        // Short timestamp (right-aligned column at the row edge).
        ps.hist_row_when[r] = lv_label_create(ps.hist_rows[r]);
        lv_label_set_text(ps.hist_row_when[r], "");
        lv_obj_set_width(ps.hist_row_when[r], HIST_WHEN_W);
        lv_obj_set_style_text_align(ps.hist_row_when[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(ps.hist_row_when[r], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ps.hist_row_when[r], hex_color(CLR_MUTED), 0);
        lv_obj_align(ps.hist_row_when[r], LV_ALIGN_RIGHT_MID, -HIST_RP, 0);
    }

    // Pager: prev/next arrows with a numeric "Page N / M" label between them
    // (no dots - the label scales to any page count). Same arrow geometry as the
    // programs list, just slimmer.
    {
        static constexpr int ARROW_INSET = 14;
        const int arrow_y = HIST_PAGER_CY - HIST_ARROW_H / 2;

        ps.hist_page_label = lv_label_create(ps.pnl_history);
        lv_label_set_text(ps.hist_page_label, "");
        lv_obj_set_style_text_font(ps.hist_page_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ps.hist_page_label, hex_color(CLR_MUTED), 0);
        lv_obj_set_style_text_align(ps.hist_page_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(ps.hist_page_label, 0, HIST_PAGER_CY -
                       lv_font_get_line_height(&lv_font_montserrat_14) / 2);
        lv_obj_set_width(ps.hist_page_label, SCREEN_W);
        lv_obj_add_flag(ps.hist_page_label, LV_OBJ_FLAG_HIDDEN);

        ps.hist_page_prev = make_btn(ps.pnl_history, LV_SYMBOL_LEFT,
                                     CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(ps.hist_page_prev, PROG_ARROW_W, HIST_ARROW_H);
        lv_obj_set_pos(ps.hist_page_prev, ARROW_INSET, arrow_y);
        lv_obj_set_ext_click_area(ps.hist_page_prev, 14);  // slim arrow: grow hit zone
        if (cbs.on_hist_page_prev)
            lv_obj_add_event_cb(ps.hist_page_prev, cbs.on_hist_page_prev,
                                LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(ps.hist_page_prev, LV_OBJ_FLAG_HIDDEN);

        ps.hist_page_next = make_btn(ps.pnl_history, LV_SYMBOL_RIGHT,
                                     CLR_LINE, CLR_TEXT, &lv_font_montserrat_20, 8);
        lv_obj_set_size(ps.hist_page_next, PROG_ARROW_W, HIST_ARROW_H);
        lv_obj_set_pos(ps.hist_page_next, SCREEN_W - PROG_ARROW_W - ARROW_INSET, arrow_y);
        lv_obj_set_ext_click_area(ps.hist_page_next, 14);  // slim arrow: grow hit zone
        if (cbs.on_hist_page_next)
            lv_obj_add_event_cb(ps.hist_page_next, cbs.on_hist_page_next,
                                LV_EVENT_CLICKED, nullptr);
        lv_obj_add_flag(ps.hist_page_next, LV_OBJ_FLAG_HIDDEN);
    }

    return ps;
}

// ---------------------------------------------------------------------------
// Station pill grid (rebuilt when the model changes)
// ---------------------------------------------------------------------------
void build_station_grid(PanelScreen& ps, StationModel& model) {
    for (int i = 0; i < ps.pill_count; ++i) {
        if (ps.stn_pills[i]) { lv_obj_delete(ps.stn_pills[i]); ps.stn_pills[i] = nullptr; }
        ps.stn_pill_lbls[i] = nullptr;
    }
    ps.pill_count = 0;

    const auto& runnable = model.runnable_sids();
    const int n = static_cast<int>(runnable.size());
    if (n == 0) return;

    const GridLayout layout = model.layout();
    // Pill size: fit layout.cols pills in (SCREEN_W-12) with 6 px gaps.
    const int inner_w = SCREEN_W - 12;
    const int pill_w  = (inner_w - (layout.cols - 1) * 6) / layout.cols;
    const int pill_h  = 40;

    for (int i = 0; i < n && i < 24; ++i) {
        const int sid = runnable[i];
        lv_obj_t* pill = lv_btn_create(ps.grid_cont);
        lv_obj_set_size(pill, pill_w, pill_h);
        lv_obj_set_style_bg_color(pill, hex_color(CLR_LINE), 0);
        lv_obj_set_style_radius(pill, 8, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        set_obj_id(pill, sid);

        char num[8];
        snprintf(num, sizeof(num), "%d", sid + 1);
        lv_obj_t* lbl = lv_label_create(pill);
        lv_label_set_text(lbl, num);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, hex_color(CLR_TEXT), 0);
        lv_obj_center(lbl);

        if (ps.cbs.on_pill)
            lv_obj_add_event_cb(pill, ps.cbs.on_pill, LV_EVENT_CLICKED, nullptr);
        ps.stn_pill_lbls[ps.pill_count] = lbl;
        ps.stn_pills[ps.pill_count++]   = pill;
    }
}

// ---------------------------------------------------------------------------
// Update: sync every widget from the current PanelView
// ---------------------------------------------------------------------------
void update_panel_screen(PanelScreen& ps,
                         const PanelView& v,
                         StationModel& model,
                         const JpData& programs,
                         const HistoryView& history,
                         const HostStatus& host) {
    char buf[80];

    const bool show_syncing = v.show_syncing;
    const bool running      = (v.phase == Phase::Running);
    const bool prog_running = (v.phase == Phase::ProgramRunning);
    const bool any_running  = running || prog_running;
    const bool show_programs = v.showing_programs_list;
    const bool show_history  = v.showing_history;

    const TopBarState top_state = resolve_top_bar_state(v);
    const std::string status_text = top_bar_status_text(v);
    uint32_t drop_color = CLR_TEAL;
    uint32_t name_color = CLR_TEXT;
    uint32_t status_color = CLR_TEAL;
    uint32_t rule_color = CLR_TEALDIM;
    switch (top_state) {
        case TopBarState::Syncing:
        case TopBarState::Reconnecting:
            drop_color = CLR_AMBER;
            name_color = CLR_MUTED;
            status_color = CLR_AMBER;
            rule_color = CLR_AMBER;
            break;
        case TopBarState::AuthError:
        case TopBarState::Offline:
            drop_color = CLR_RED;
            name_color = CLR_RED;
            status_color = CLR_RED;
            rule_color = CLR_RED;
            break;
        case TopBarState::Disabled:
            status_color = CLR_RED;
            rule_color = CLR_RED;
            break;
        case TopBarState::RainDelay:
        case TopBarState::Clean:
            break;
    }

    lv_label_set_text(ps.tb.lbl_name, v.controller_identity.c_str());
    lv_label_set_text(ps.tb.lbl_status, status_text.c_str());
    lv_obj_set_style_text_color(ps.tb.lbl_drop, hex_color(drop_color), 0);
    lv_obj_set_style_text_color(ps.tb.lbl_name, hex_color(name_color), 0);
    lv_obj_set_style_text_color(ps.tb.lbl_status, hex_color(status_color), 0);
    lv_obj_set_style_bg_color(ps.tb.top_accent, hex_color(rule_color), 0);
    update_drop_pulse(ps, top_state == TopBarState::Syncing ||
                          top_state == TopBarState::Reconnecting);

    obj_set_hidden(ps.tb.current.box, !v.has_current);
    if (v.has_current) {
        snprintf(buf, sizeof(buf), "%d", v.current_ma);
        lv_label_set_text(ps.tb.current.val, buf);
        lv_obj_set_style_text_color(
            ps.tb.current.val,
            hex_color(v.current_ma > 0 ? CLR_TEXT : CLR_MUTED), 0);
    }

    // Drawn RSSI bar meters. Panel signal comes from the firmware (host); the
    // controller signal comes from the pure view model.
    update_sig_meter(ps.tb.sig_panel, host.panel_rssi_bars, host.panel_connected);
    update_sig_meter(ps.tb.sig_ctrl, rssi_to_bars(v.ctrl_rssi),
                     v.link == LinkState::Connected &&
                         top_state != TopBarState::Syncing);

    // Battery gauge: the firmware samples the ADC and passes the smoothed
    // state-of-charge. Always shown - an absent cell floats the sense node high
    // (indistinguishable from full), so there is no reliable "no battery" state.
    update_batt_glyph(ps.tb.batt, host.battery_percent, host.battery_tier);

    // Phase visibility. The shared status panel + action row show for BOTH the
    // manual and program run; the right column (settings+grid vs queue list) is
    // the only per-mode difference.
    obj_set_hidden(ps.pnl_idle,       any_running || show_programs || show_history);
    obj_set_hidden(ps.pnl_status,     !any_running);
    obj_set_hidden(ps.pnl_prog_qlist, !prog_running);
    obj_set_hidden(ps.pnl_programs,   !show_programs);
    obj_set_hidden(ps.pnl_history,    !show_history);
    obj_set_hidden(ps.pnl_right,      show_programs || show_history || prog_running);
    // The Programs/History entry buttons live in the right panel; hide them
    // while a manual station is running (the overlays only open from idle).
    obj_set_hidden(ps.btn_programs,   any_running);
    obj_set_hidden(ps.btn_history,    any_running);
    obj_set_hidden(ps.btn_next,       !any_running);
    obj_set_hidden(ps.btn_pause,      !any_running);
    obj_set_hidden(ps.btn_stop,       !any_running);
    // Station grid is only useful for idle (start a station) and manual runs
    // (jump to a station). Hide it on the programs list and during program runs.
    obj_set_hidden(ps.grid_cont,      show_programs || prog_running);
    obj_set_hidden(ps.lbl_grid_title, show_programs || prog_running);

    if (!any_running && !show_programs) {
        if (show_syncing) {
            snprintf(buf, sizeof(buf), "%s Syncing...", LV_SYMBOL_REFRESH);
            lv_label_set_text(ps.lbl_idle_head, buf);
            lv_label_set_text(ps.lbl_idle_sub, "Waiting for the controller to confirm");
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_AMBER), 0);
        } else if (v.link == LinkState::AuthError) {
            snprintf(buf, sizeof(buf), "%s Auth error", LV_SYMBOL_WARNING);
            lv_label_set_text(ps.lbl_idle_head, buf);
            snprintf(buf, sizeof(buf), "Invalid credentials for controller at %s",
                     (host.host_name && host.host_name[0]) ? host.host_name : "controller");
            lv_label_set_text(ps.lbl_idle_sub, buf);
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_RED), 0);
        } else if (v.link == LinkState::Offline) {
            snprintf(buf, sizeof(buf), "%s Controller offline", LV_SYMBOL_WARNING);
            lv_label_set_text(ps.lbl_idle_head, buf);
            snprintf(buf, sizeof(buf), "Cannot reach controller at %s",
                     (host.host_name && host.host_name[0]) ? host.host_name : "controller");
            lv_label_set_text(ps.lbl_idle_sub, buf);
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_RED), 0);
        } else if (v.link == LinkState::Reconnecting) {
            snprintf(buf, sizeof(buf), "%s Reconnecting...", LV_SYMBOL_REFRESH);
            lv_label_set_text(ps.lbl_idle_head, buf);
            lv_label_set_text(ps.lbl_idle_sub, "Waiting for the controller to respond");
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_AMBER), 0);
        } else if (!v.station_list_loaded) {
            lv_label_set_text(ps.lbl_idle_head, "Loading stations...");
            lv_label_set_text(ps.lbl_idle_sub, "Waiting for the controller to respond");
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_MUTED), 0);
        } else {
            snprintf(buf, sizeof(buf), "%s Select a station", LV_SYMBOL_DOWN);
            lv_label_set_text(ps.lbl_idle_head, buf);
            lv_label_set_text(ps.lbl_idle_sub, "Tap a station below to start");
            lv_obj_set_style_text_color(ps.lbl_idle_sub, hex_color(CLR_TEAL), 0);
        }
    }

    // ---- Shared running status render (manual + program) ---------------
    // ONE render path drives the shared status stack (eyebrow / name /
    // countdown / paused block) and the Pause button label for BOTH modes.
    // Only the eyebrow and station-name TEXT differ by mode.
    if (any_running) {
        const auto& stns = model.stations();

        if (prog_running) {
            const auto& pr = v.prog_run;
            // Eyebrow: identified program -> "STATION N OF M"; unidentified /
            // external run -> honest "N STATIONS LEFT".
            const std::string eyebrow = program_run_eyebrow(
                pr.program_index, pr.current_station_number, pr.station_count);
            lv_label_set_text(ps.lbl_eyebrow, eyebrow.c_str());

            // Headline: current station name.
            if (pr.current_sid >= 0 &&
                    pr.current_sid < static_cast<int>(stns.size())) {
                lv_label_set_text(ps.lbl_stn_name, stns[pr.current_sid].name.c_str());
            } else {
                lv_label_set_text(ps.lbl_stn_name, "Finishing...");
            }
        } else {
            // Manual run: "STATION N" + the running zone name.
            snprintf(buf, sizeof(buf), "STATION %d", v.running_sid + 1);
            lv_label_set_text(ps.lbl_eyebrow, buf);
            const char* name = (v.running_sid >= 0 &&
                                v.running_sid < static_cast<int>(stns.size()))
                                ? stns[v.running_sid].name.c_str() : "Station";
            lv_label_set_text(ps.lbl_stn_name, name);
        }

        // Countdown (M:SS) - frozen while paused (view holds countdown_s).
        char cd[16];
        fmt_countdown(cd, v.countdown_s);
        lv_label_set_text(ps.lbl_countdown, cd);

        // Option B paused block: "PAUSED" + zero-padded "Resumes in MM:SS".
        if (v.paused) {
            int rs = v.pause_remaining_s;
            if (rs < 0) rs = 0;
            snprintf(buf, sizeof(buf), "Resumes in %02d:%02d", rs / 60, rs % 60);
            lv_label_set_text(ps.lbl_resume, buf);
            obj_set_hidden(ps.lbl_paused, false);
            obj_set_hidden(ps.lbl_resume, false);
        } else {
            obj_set_hidden(ps.lbl_paused, true);
            obj_set_hidden(ps.lbl_resume, true);
        }

        // Pause button toggles to Resume while paused.
        if (ps.lbl_pause_txt) {
            lv_label_set_text(ps.lbl_pause_txt, v.paused ? "Resume" : "Pause");
        }
    }

    // M9: Program queue view (right-side queue list, program-only)
    if (prog_running) {
        const auto& pr = v.prog_run;
        const auto& progs = programs.programs;
        const auto& stns = model.stations();

        // Program name - shown only as the queue-list header (right panel).
        const char* prog_name = "Station Queue";  // generic until identified
        if (pr.program_index >= 0 &&
                pr.program_index < static_cast<int>(progs.size())) {
            prog_name = progs[pr.program_index].name.c_str();
        }

        // ---- Right-side queue list -------------------------------------
        lv_label_set_text(ps.lbl_qlist_hdr, prog_name);
        {
            int tr = pr.total_remaining_seconds;
            if (tr < 0) tr = 0;
            snprintf(buf, sizeof(buf), "%d:%02d left", tr / 60, tr % 60);
            lv_label_set_text(ps.lbl_qlist_total, buf);
        }

        // Window the queue around the current station, keeping ~2 completed
        // rows visible above it for context. Chevron hints flag hidden rows.
        const auto& q = pr.queue;
        const int qn = static_cast<int>(q.size());
        int cur = 0;
        for (int i = 0; i < qn; ++i) {
            if (q[i].sid == pr.current_sid) { cur = i; break; }
        }
        int win_start = 0;
        if (qn > MAX_QROWS) {
            win_start = cur - 2;  // keep two done rows visible above current
            if (win_start < 0) win_start = 0;
            if (win_start > qn - MAX_QROWS) win_start = qn - MAX_QROWS;
        }
        const bool more_above = win_start > 0;
        const bool more_below = (win_start + MAX_QROWS) < qn;
        obj_set_hidden(ps.qfade_top, !more_above);
        obj_set_hidden(ps.qfade_bottom, !more_below);

        for (int r = 0; r < MAX_QROWS; ++r) {
            const int qi = win_start + r;
            if (qi < qn) {
                const auto& e = q[qi];
                const bool is_current = (e.sid == pr.current_sid);
                const bool is_done = e.done;

                // Current station shows a play glyph while running; when the
                // queue is paused it flips to a pause glyph so the list mirrors
                // the paused state (and the "Resume" button / amber status word).
                const char* current_mark =
                    v.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
                const char* mark = is_current ? current_mark
                                   : (is_done ? LV_SYMBOL_OK : "");
                lv_label_set_text(ps.qrow_mark[r], mark);
                lv_obj_set_style_text_color(ps.qrow_mark[r],
                    hex_color(is_current ? CLR_TEAL : CLR_MUTED), 0);

                const char* nm =
                    (e.sid >= 0 && e.sid < static_cast<int>(stns.size()))
                    ? stns[e.sid].name.c_str() : "Station";
                lv_label_set_text(ps.qrow_name[r], nm);
                lv_obj_set_style_text_color(ps.qrow_name[r],
                    hex_color(is_done ? CLR_MUTED : CLR_TEXT), 0);

                // Queue rows always show the station's FULL configured
                // duration (static) - the live per-station countdown lives in
                // the big ticker on the left, so nothing counts down here.
                int secs = e.total_seconds;
                if (secs < 0) secs = 0;
                snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
                lv_label_set_text(ps.qrow_dur[r], buf);
                lv_obj_set_style_text_color(ps.qrow_dur[r],
                    hex_color(is_current ? CLR_TEAL : CLR_MUTED), 0);

                obj_set_hidden(ps.qrow_mark[r], false);
                obj_set_hidden(ps.qrow_name[r], false);
                obj_set_hidden(ps.qrow_dur[r], false);
            } else {
                obj_set_hidden(ps.qrow_mark[r], true);
                obj_set_hidden(ps.qrow_name[r], true);
                obj_set_hidden(ps.qrow_dur[r], true);
            }
        }
    }

    // M9: Programs list content
    if (show_programs) {
        const auto& jp = programs;
        const int nprogs = static_cast<int>(jp.programs.size());
        const int page   = v.prog_list_page;
        const int start  = page * MAX_PROG_ROWS;
        const int total_pages = (nprogs + MAX_PROG_ROWS - 1) / MAX_PROG_ROWS;

        for (int r = 0; r < MAX_PROG_ROWS; ++r) {
            const int idx = start + r;
            if (idx < nprogs) {
                const auto& prog = jp.programs[idx];
                const int pid = idx + 1;
                const bool en = prog.enabled;

                // Enable/disable icon + name. Different SHAPES per state so
                // disabled doesn't just read as a dimmer "on": a power glyph
                // (teal) when enabled, a flat dash (muted) when disabled. The
                // dimmed name and Enable/Disable button reinforce it.
                lv_label_set_text(ps.prog_row_icon[r],
                                  en ? LV_SYMBOL_POWER : LV_SYMBOL_MINUS);
                lv_obj_set_style_text_color(ps.prog_row_icon[r],
                    hex_color(en ? CLR_TEAL : CLR_MUTED), 0);
                lv_label_set_text(ps.prog_row_name[r], prog.name.c_str());
                lv_obj_set_style_text_color(ps.prog_row_name[r],
                    hex_color(en ? CLR_TEXT : CLR_MUTED), 0);

                // Toggle button label
                lv_obj_t* tog_lbl =
                    static_cast<lv_obj_t*>(lv_obj_get_child(ps.prog_row_btn_toggle[r], 0));
                if (tog_lbl) lv_label_set_text(tog_lbl, en ? "Disable" : "Enable");
                set_obj_id(ps.prog_row_btn_toggle[r], pid);

                // Next-run + zones/runtime meta line.
                {
                    char nr_buf[64];
                    char meta[32];
                    // "N zones - MM min" summary from /jp durations.
                    const int zones = prog.station_count();
                    const int mins  = (prog.total_seconds() + 59) / 60;
                    snprintf(meta, sizeof(meta),
                             "%d zones " LV_SYMBOL_BULLET " %d min", zones, mins);

                    // Compute the next run from the schedule regardless of the
                    // enabled flag (next_run() short-circuits on disabled, so
                    // evaluate an enabled copy to show *when it would run*).
                    auto sched = prog;
                    sched.enabled = true;
                    const long nr = next_run(sched, v.ctrl_devt,
                                             v.sunrise_min, v.sunset_min);
                    char when[32];
                    if (nr < 0) {
                        snprintf(when, sizeof(when), "Not scheduled");
                    } else {
                        // Calendar-day delta (not elapsed 24h periods).
                        const long nr_day   = nr / 86400;
                        const long dev_day  = v.ctrl_devt / 86400;
                        const long days_until = nr_day - dev_day;
                        const long tod = nr % 86400;
                        int h = static_cast<int>(tod / 3600);
                        const int m = static_cast<int>((tod % 3600) / 60);
                        const char* ap = (h < 12) ? "AM" : "PM";
                        int h12 = h % 12; if (h12 == 0) h12 = 12;

                        if (days_until <= 0) {
                            snprintf(when, sizeof(when), "Today %d:%02d %s",
                                     h12, m, ap);
                        } else if (days_until == 1) {
                            snprintf(when, sizeof(when), "Tomorrow %d:%02d %s",
                                     h12, m, ap);
                        } else if (days_until < 7) {
                            // Weekday abbrev; 1970-01-01 was a Thursday (=4).
                            static const char* kWd[7] =
                                {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                            const int wd = static_cast<int>((nr_day + 4) % 7);
                            snprintf(when, sizeof(when), "%s %d:%02d %s",
                                     kWd[wd], h12, m, ap);
                        } else {
                            snprintf(when, sizeof(when), "+%ld days %d:%02d %s",
                                     days_until, h12, m, ap);
                        }
                    }

                    // Next-run + meta on one line. State is shown by the icon
                    // + dimming, so disabled programs are NOT tagged in text -
                    // we still compute when they *would* run for reference.
                    snprintf(nr_buf, sizeof(nr_buf),
                             "%s " LV_SYMBOL_BULLET " %s", when, meta);
                    lv_label_set_text(ps.prog_row_next[r], nr_buf);
                }

                // Run button pid
                set_obj_id(ps.prog_row_btn_run[r], pid);

                obj_set_hidden(ps.prog_rows[r], false);
            } else {
                obj_set_hidden(ps.prog_rows[r], true);
            }
        }

        // Pager: dots (indicators) + arrows, only when >1 page.
        const bool need_pager = total_pages > 1;
        // Re-centre the visible dot cluster between the arrows (screen centre),
        // sized to the live page count so it stays centred regardless of how
        // many pages exist.
        const int dots_w = need_pager
            ? total_pages * PROG_DOT_W + (total_pages - 1) * PROG_DOT_GAP
            : 0;
        const int dots_x0 = (SCREEN_W - dots_w) / 2;
        for (int d = 0; d < MAX_PROG_PAGES; ++d) {
            if (!ps.prog_page_dots[d]) continue;
            obj_set_hidden(ps.prog_page_dots[d], !need_pager || d >= total_pages);
            if (need_pager && d < total_pages) {
                lv_obj_set_x(ps.prog_page_dots[d],
                             dots_x0 + d * (PROG_DOT_W + PROG_DOT_GAP));
                lv_obj_set_style_bg_color(ps.prog_page_dots[d],
                    hex_color(d == page ? CLR_TEAL : CLR_LINE), 0);
            }
        }
        if (ps.prog_page_prev && ps.prog_page_next) {
            obj_set_hidden(ps.prog_page_prev, !need_pager);
            obj_set_hidden(ps.prog_page_next, !need_pager);
            if (need_pager) {
                // Dim (but keep tappable - setter clamps) at the ends. make_btn
                // set the color on the child label, so target that.
                lv_obj_t* pl = lv_obj_get_child(ps.prog_page_prev, 0);
                lv_obj_t* nl = lv_obj_get_child(ps.prog_page_next, 0);
                if (pl) lv_obj_set_style_text_color(pl,
                    hex_color(page <= 0 ? CLR_MUTED : CLR_TEXT), 0);
                if (nl) lv_obj_set_style_text_color(nl,
                    hex_color(page >= total_pages - 1 ? CLR_MUTED : CLR_TEXT), 0);
            }
        }
    }

    // #127: History list content
    if (show_history) {
        const int count = history.count;
        const int total_pages =
            count > 0 ? (count + MAX_HIST_ROWS - 1) / MAX_HIST_ROWS : 0;
        int page = history.page;
        if (page < 0) page = 0;
        if (total_pages > 0 && page > total_pages - 1) page = total_pages - 1;
        const int start = page * MAX_HIST_ROWS;

        obj_set_hidden(ps.lbl_hist_empty, count > 0);

        for (int r = 0; r < MAX_HIST_ROWS; ++r) {
            const int idx = start + r;
            if (count > 0 && idx < count) {
                const HistoryEntry& e = history.entries[idx];

                // Kind -> colours (no icon; kind reads from the tag + tint).
                // Runs read bright (teal/lede); events (rain delay, sensors) are
                // dimmed so the log scans as "runs with the occasional event".
                uint32_t name_col = CLR_TEXT;
                uint32_t dur_col  = CLR_LEDE;
                switch (e.kind) {
                    case HistoryEntry::ProgramRun:
                    case HistoryEntry::ManualRun:
                    case HistoryEntry::RunOnce:
                        break;
                    case HistoryEntry::RainDelay:
                        name_col = CLR_AMBER; dur_col = CLR_MUTED; break;
                    case HistoryEntry::Sensor1:
                    case HistoryEntry::Sensor2:
                        name_col = CLR_MUTED; dur_col = CLR_MUTED; break;
                }

                // Context tag: program name for a program run, "Manual" for a
                // manual run (default when the fixture leaves tag null). When a
                // tag is present the name column is narrowed so the two never
                // overlap and the name ellipsizes first.
                const char* tag = e.tag;
                if (!tag && e.kind == HistoryEntry::ManualRun) tag = "Manual";
                const bool has_tag = tag && tag[0];
                lv_obj_set_width(ps.hist_row_name[r],
                                 has_tag ? HIST_NAME_W_TAG : HIST_NAME_W);
                if (has_tag) lv_label_set_text(ps.hist_row_tag[r], tag);
                obj_set_hidden(ps.hist_row_tag[r], !has_tag);
                lv_label_set_text(ps.hist_row_name[r], e.name ? e.name : "");
                lv_obj_set_style_text_color(ps.hist_row_name[r], hex_color(name_col), 0);

                // Duration: compact ("45s" / "8m" / "1h05m"); blank for events
                // (rain delay, sensor trips) which have no run duration.
                const bool is_run = e.kind == HistoryEntry::ProgramRun ||
                                    e.kind == HistoryEntry::ManualRun ||
                                    e.kind == HistoryEntry::RunOnce;
                if (!is_run || e.dur_s == 0) {
                    lv_label_set_text(ps.hist_row_dur[r], "");
                } else if (e.dur_s < 60) {
                    snprintf(buf, sizeof(buf), "%us", e.dur_s);
                    lv_label_set_text(ps.hist_row_dur[r], buf);
                } else if (e.dur_s < 3600) {
                    snprintf(buf, sizeof(buf), "%um", (e.dur_s + 30) / 60);
                    lv_label_set_text(ps.hist_row_dur[r], buf);
                } else {
                    const unsigned h = e.dur_s / 3600;
                    const unsigned m = (e.dur_s % 3600) / 60;
                    snprintf(buf, sizeof(buf), "%uh%02um", h, m);
                    lv_label_set_text(ps.hist_row_dur[r], buf);
                }
                lv_obj_set_style_text_color(ps.hist_row_dur[r], hex_color(dur_col), 0);

                lv_label_set_text(ps.hist_row_when[r], e.when ? e.when : "");

                obj_set_hidden(ps.hist_rows[r], false);
            } else {
                obj_set_hidden(ps.hist_rows[r], true);
            }
        }

        // Pager: numeric "Page N / M" label + arrows, only when >1 page.
        const bool need_pager = total_pages > 1;
        if (ps.hist_page_label) {
            obj_set_hidden(ps.hist_page_label, !need_pager);
            if (need_pager) {
                snprintf(buf, sizeof(buf), "Page %d / %d", page + 1, total_pages);
                lv_label_set_text(ps.hist_page_label, buf);
            }
        }
        if (ps.hist_page_prev && ps.hist_page_next) {
            obj_set_hidden(ps.hist_page_prev, !need_pager);
            obj_set_hidden(ps.hist_page_next, !need_pager);
            if (need_pager) {
                lv_obj_t* pl = lv_obj_get_child(ps.hist_page_prev, 0);
                lv_obj_t* nl = lv_obj_get_child(ps.hist_page_next, 0);
                if (pl) lv_obj_set_style_text_color(pl,
                    hex_color(page <= 0 ? CLR_MUTED : CLR_TEXT), 0);
                if (nl) lv_obj_set_style_text_color(nl,
                    hex_color(page >= total_pages - 1 ? CLR_MUTED : CLR_TEXT), 0);
            }
        }
    }

    // Run-time value (only meaningful when right panel is visible)
    snprintf(buf, sizeof(buf), "%d:%02d", v.run_time_s / 60, v.run_time_s % 60);
    lv_label_set_text(ps.lbl_rt_value, buf);

    // Auto-advance switch
    const bool sw_on = lv_obj_has_state(ps.sw_auto_adv, LV_STATE_CHECKED);
    if (v.auto_advance != sw_on) {
        if (v.auto_advance) lv_obj_add_state(ps.sw_auto_adv, LV_STATE_CHECKED);
        else                lv_obj_clear_state(ps.sw_auto_adv, LV_STATE_CHECKED);
    }

    // Grid label (kept consistent as "STATIONS" in both idle and running).
    lv_label_set_text(ps.lbl_grid_title, "STATIONS");

    // Pill highlights
    for (int i = 0; i < ps.pill_count; ++i) {
        if (!ps.stn_pills[i]) continue;
        const int sid = get_obj_id(ps.stn_pills[i]);
        const bool active = (running && sid == v.running_sid) ||
                            (prog_running && sid == v.prog_run.current_sid);
        lv_obj_set_style_bg_color(ps.stn_pills[i],
            hex_color(active ? CLR_TEAL : CLR_LINE), 0);
        if (ps.stn_pill_lbls[i]) {
            lv_obj_set_style_text_color(ps.stn_pill_lbls[i],
                hex_color(active ? CLR_BG : CLR_TEXT), 0);
        }
    }

    // Sleep overlay. (The backlight side-effect stays in the firmware caller.)
    obj_set_hidden(ps.sleep_overlay, !v.sleeping);
}

}  // namespace ui
}  // namespace osp
