// Small portable LVGL widget helpers shared across the lib/ui screens.
//
// LVGL-only (no Arduino, no TFT_eSPI, no network) so the firmware and the host
// `sim` link the same code. Extracted verbatim from src/main.cpp.
#pragma once

#include <cstdint>

#include <lvgl.h>

#include "ui_theme.h"  // hex_color

namespace osp {
namespace ui {

// Create a rounded button with a centred single-line label. Mirrors the helper
// that lived in src/main.cpp: bg/fg are 0xRRGGBB tokens, font + radius default
// to the common small-button values. The label is child 0 of the button so
// callers can recolour/retext it via lv_obj_get_child(btn, 0).
inline lv_obj_t* make_btn(lv_obj_t* parent, const char* text,
                          uint32_t bg, uint32_t fg,
                          const lv_font_t* font = &lv_font_montserrat_14,
                          int radius = 6) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, hex_color(bg), 0);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, hex_color(fg), 0);
    lv_obj_center(lbl);
    return btn;
}

// Store/retrieve a small int id (station sid or program pid) as lv_obj
// user_data (void*). Used by the station grid pills and the program rows so an
// event callback can recover which station/program was tapped.
inline void set_obj_id(lv_obj_t* obj, int id) {
    lv_obj_set_user_data(obj, reinterpret_cast<void*>(static_cast<intptr_t>(id)));
}
inline int get_obj_id(lv_obj_t* obj) {
    return static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(obj)));
}

}  // namespace ui
}  // namespace osp
