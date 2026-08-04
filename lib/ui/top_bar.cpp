// Portable LVGL top bar implementation (seed of lib/ui). Extracted verbatim
// from src/main.cpp so firmware and the host `sim` render identical pixels.
#include "top_bar.h"

#include "station_model.h"  // osp::display_bars
#include "ui_theme.h"

namespace osp {
namespace ui {

// ---------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------

static CurrentSlot build_current_slot(lv_obj_t* parent) {
    CurrentSlot slot;
    slot.box = lv_obj_create(parent);
    lv_obj_remove_style_all(slot.box);
    lv_obj_set_style_bg_opa(slot.box, LV_OPA_TRANSP, 0);
    lv_obj_set_size(slot.box, 46, TOP_H);
    lv_obj_set_style_pad_column(slot.box, 2, 0);
    lv_obj_clear_flag(slot.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(slot.box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(slot.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slot.box, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    slot.val = lv_label_create(slot.box);
    lv_obj_set_width(slot.val, 26);
    lv_label_set_long_mode(slot.val, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(slot.val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(slot.val, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(slot.val, hex_color(CLR_MUTED), 0);
    lv_label_set_text(slot.val, "0");

    slot.unit = lv_label_create(slot.box);
    lv_obj_set_width(slot.unit, 18);
    lv_label_set_long_mode(slot.unit, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(slot.unit, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(slot.unit, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(slot.unit, hex_color(CLR_MUTED), 0);
    lv_label_set_text(slot.unit, "mA");

    return slot;
}

// Build a compact drawn RSSI meter into `parent` (a pre-created flex-row group).
// Layout: "P"/"C" label followed by 4 ascending bar rectangles.
static SigMeter build_sig_meter(lv_obj_t* parent, const char* txt) {
    SigMeter m;

    // Flex-row outer container - transparent, no border, no padding.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_column(row, 3, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // "P" / "C" text label.
    lv_obj_t* lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl, hex_color(CLR_MUTED), 0);

    // 22x10 sub-container for the 4 ascending bar rects (absolute layout).
    lv_obj_t* bc = lv_obj_create(row);
    lv_obj_remove_style_all(bc);
    lv_obj_set_style_bg_opa(bc, LV_OPA_TRANSP, 0);
    lv_obj_set_size(bc, 22, 10);   // width: 4 bars x4 px + 3 gaps x2 px = 22; height: tallest bar = 10
    lv_obj_clear_flag(bc, LV_OBJ_FLAG_SCROLLABLE);

    static const int bh[4] = {4, 6, 8, 10};
    for (int i = 0; i < 4; ++i) {
        m.bars[i] = lv_obj_create(bc);
        lv_obj_set_size(m.bars[i], 4, bh[i]);
        lv_obj_set_style_border_width(m.bars[i], 0, 0);
        lv_obj_set_style_radius(m.bars[i], 1, 0);
        lv_obj_set_pos(m.bars[i], i * 6, 10 - bh[i]);  // bottom-align bars
        lv_obj_set_style_bg_color(m.bars[i], hex_color(CLR_LINE), 0);
        lv_obj_set_style_bg_opa(m.bars[i], LV_OPA_COVER, 0);
    }

    return m;
}

// Battery pictogram geometry. Body is 18x11 with a 1 px border + 1 px pad, so
// the inner fill spans BATT_FILL_MAX_W x 7 px. A 2 px nub hangs off the right.
static constexpr int BATT_BODY_W    = 18;
static constexpr int BATT_BODY_H    = 11;
static constexpr int BATT_FILL_MAX_W = BATT_BODY_W - 2 /*border*/ - 2 /*pad*/;  // 14

// Build the battery gauge (pictogram + percent) into `parent`.
static BattGlyph build_batt_glyph(lv_obj_t* parent) {
    BattGlyph g;

    // Flex-row: pictogram + percent label, small gap.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_size(row, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Pictogram holder (body + nub in absolute layout).
    lv_obj_t* pic = lv_obj_create(row);
    g.pic = pic;
    lv_obj_remove_style_all(pic);
    lv_obj_set_style_bg_opa(pic, LV_OPA_TRANSP, 0);
    lv_obj_set_size(pic, BATT_BODY_W + 2, BATT_BODY_H);  // +2 for the nub
    lv_obj_clear_flag(pic, LV_OBJ_FLAG_SCROLLABLE);

    // Body outline.
    g.body = lv_obj_create(pic);
    lv_obj_remove_style_all(g.body);
    lv_obj_set_size(g.body, BATT_BODY_W, BATT_BODY_H);
    lv_obj_set_pos(g.body, 0, 0);
    lv_obj_set_style_bg_opa(g.body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g.body, 1, 0);
    lv_obj_set_style_border_color(g.body, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_radius(g.body, 2, 0);
    lv_obj_set_style_pad_all(g.body, 1, 0);
    lv_obj_clear_flag(g.body, LV_OBJ_FLAG_SCROLLABLE);

    // Terminal nub.
    g.nub = lv_obj_create(pic);
    lv_obj_remove_style_all(g.nub);
    lv_obj_set_size(g.nub, 2, 5);
    lv_obj_set_pos(g.nub, BATT_BODY_W, (BATT_BODY_H - 5) / 2);
    lv_obj_set_style_bg_color(g.nub, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_bg_opa(g.nub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.nub, 1, 0);
    lv_obj_clear_flag(g.nub, LV_OBJ_FLAG_SCROLLABLE);

    // Inner fill (left-anchored, width scaled by %).
    g.fill = lv_obj_create(g.body);
    lv_obj_remove_style_all(g.fill);
    lv_obj_set_size(g.fill, BATT_FILL_MAX_W, 7);
    lv_obj_set_pos(g.fill, 0, 0);
    lv_obj_set_style_bg_color(g.fill, hex_color(CLR_TEAL), 0);
    lv_obj_set_style_bg_opa(g.fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.fill, 1, 0);
    lv_obj_clear_flag(g.fill, LV_OBJ_FLAG_SCROLLABLE);

    g.charge = lv_label_create(pic);
    lv_obj_set_style_text_font(g.charge, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g.charge, hex_color(CLR_TEAL), 0);
    lv_label_set_text(g.charge, LV_SYMBOL_CHARGE);
    lv_obj_center(g.charge);
    lv_obj_add_flag(g.charge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.charge, LV_OBJ_FLAG_SCROLLABLE);

    // Percent label.
    g.pct = lv_label_create(row);
    lv_obj_set_width(g.pct, 30);
    lv_obj_set_style_text_align(g.pct, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(g.pct, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(g.pct, hex_color(CLR_LEDE), 0);
    lv_label_set_text(g.pct, "--%");

    return g;
}

TopBar build_top_bar(lv_obj_t* scr) {
    TopBar t;

    lv_obj_t* bar = lv_obj_create(scr);
    t.bar = bar;
    lv_obj_set_size(bar, SCREEN_W, TOP_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, hex_color(CLR_BG), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 2, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* left_group = lv_obj_create(bar);
    lv_obj_remove_style_all(left_group);
    lv_obj_set_style_bg_opa(left_group, LV_OPA_TRANSP, 0);
    lv_obj_set_size(left_group, 220, TOP_H);
    lv_obj_set_style_pad_column(left_group, 6, 0);
    lv_obj_clear_flag(left_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(left_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_group, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(left_group, LV_ALIGN_LEFT_MID, 4, 0);

    t.lbl_drop = lv_label_create(left_group);
    lv_obj_set_style_text_font(t.lbl_drop, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t.lbl_drop, hex_color(CLR_TEAL), 0);
    lv_label_set_text(t.lbl_drop, LV_SYMBOL_TINT);

    t.lbl_name = lv_label_create(left_group);
    lv_obj_set_size(t.lbl_name, 1, 14);
    lv_obj_set_flex_grow(t.lbl_name, 1);
    lv_label_set_long_mode(t.lbl_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(t.lbl_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t.lbl_name, hex_color(CLR_TEXT), 0);
    lv_label_set_text(t.lbl_name, "controller");

    t.lbl_status = lv_label_create(left_group);
    lv_obj_set_style_text_font(t.lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t.lbl_status, 1, 0);
    lv_obj_set_style_text_color(t.lbl_status, hex_color(CLR_TEAL), 0);
    lv_label_set_text(t.lbl_status, "");

    // Fixed right cluster: current, divider, panel/controller signal, battery.
    lv_obj_t* sig_group = lv_obj_create(bar);
    lv_obj_remove_style_all(sig_group);
    lv_obj_set_style_bg_opa(sig_group, LV_OPA_TRANSP, 0);
    lv_obj_set_size(sig_group, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_style_pad_column(sig_group, 12, 0);
    lv_obj_clear_flag(sig_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(sig_group, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(sig_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sig_group, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(sig_group, LV_ALIGN_RIGHT_MID, -4, 0);

    t.current = build_current_slot(sig_group);
    obj_set_hidden(t.current.box, true);

    lv_obj_t* divider = lv_obj_create(sig_group);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 2, 16);
    lv_obj_set_style_bg_color(divider, hex_color(CLR_TEALDIM), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    t.sig_panel = build_sig_meter(sig_group, "P");
    t.sig_ctrl  = build_sig_meter(sig_group, "C");
    t.batt      = build_batt_glyph(sig_group);

    t.top_accent = lv_obj_create(scr);
    lv_obj_remove_style_all(t.top_accent);
    lv_obj_set_size(t.top_accent, SCREEN_W, 3);
    lv_obj_set_pos(t.top_accent, 0, TOP_H - 3);
    lv_obj_set_style_bg_color(t.top_accent, hex_color(CLR_TEALDIM), 0);
    lv_obj_set_style_bg_opa(t.top_accent, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t.top_accent, 0, 0);
    lv_obj_clear_flag(t.top_accent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(t.top_accent);

    return t;
}

// ---------------------------------------------------------------------------
// Updates
// ---------------------------------------------------------------------------

void update_sig_meter(const SigMeter& m, int quality, bool connected) {
    const int n = osp::display_bars(quality, connected);
    const uint32_t fill_clr = (quality >= 3) ? CLR_TEAL
                            : (quality >= 1)  ? CLR_AMBER
                                              : CLR_RED;
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(m.bars[i],
            hex_color(i < n ? fill_clr : CLR_LINE), 0);
    }
}

void update_batt_glyph(const BattGlyph& g, int percent, osp::BatteryTier tier,
                       osp::PowerSource source) {
    if (!g.body) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const bool ext = osp::power_is_external(source);
    obj_set_hidden(g.body, ext);
    obj_set_hidden(g.nub, ext);
    obj_set_hidden(g.fill, ext);
    obj_set_hidden(g.charge, !ext);

    const uint32_t clr = (tier == osp::BatteryTier::Healthy) ? CLR_TEAL
                       : (tier == osp::BatteryTier::Low)     ? CLR_AMBER
                                                             : CLR_RED;
    lv_obj_set_style_border_color(g.body, hex_color(clr), 0);
    lv_obj_set_style_bg_color(g.nub, hex_color(clr), 0);
    lv_obj_set_style_bg_color(g.fill, hex_color(clr), 0);
    lv_obj_set_style_text_color(
        g.pct, hex_color(tier == osp::BatteryTier::Healthy ? CLR_LEDE : clr), 0);

    // Fill width tracks %, but keep a visible sliver whenever a cell is present.
    int w = (BATT_FILL_MAX_W * percent + 50) / 100;
    if (w < 2) w = 2;
    if (w > BATT_FILL_MAX_W) w = BATT_FILL_MAX_W;
    lv_obj_set_width(g.fill, w);

    char b[8];
    snprintf(b, sizeof(b), "%d%%", percent);
    lv_label_set_text(g.pct, b);
}

}  // namespace ui
}  // namespace osp
