// Shared UI theme tokens and helpers for the OpenSprinkler panel.
//
// Portable LVGL-only header (no Arduino, no TFT_eSPI, no network) so it compiles
// both in the ESP32 firmware and in the host `sim` environment. This is the seed
// of a future lib/ui module; today it carries just the visual tokens, tiny
// helpers and the screen/top-bar dimensions the top bar needs.
//
// The constants live at global scope (not namespaced) so the existing firmware
// code in src/main.cpp keeps referring to them unqualified. hex_color/obj_set_hidden
// are `static inline` and the CLR_* are `static constexpr`, so each translation
// unit that includes this header gets its own internal-linkage copy.
#pragma once

#include <cstdint>

#include <lvgl.h>

// ---- Screen + top-bar geometry (480x320 landscape) ------------------------
static constexpr int SCREEN_W = 480;
static constexpr int SCREEN_H = 320;
static constexpr int TOP_H    = 26;

// ---- Visual tokens (docs/01 section 5) ------------------------------------
static constexpr uint32_t CLR_BG    = 0x07100f;
static constexpr uint32_t CLR_TEXT  = 0xe9f2ef;
static constexpr uint32_t CLR_MUTED = 0x7f938f;
static constexpr uint32_t CLR_TEAL  = 0x35d0c3;
static constexpr uint32_t CLR_AMBER = 0xf2a63b;
static constexpr uint32_t CLR_RED   = 0xff5b5b;
static constexpr uint32_t CLR_LINE  = 0x1a2e2b;
static constexpr uint32_t CLR_TEALDIM = 0x1c6a64;  // accent rule / dim chip border
static constexpr uint32_t CLR_LEDE    = 0xc3d3cf;  // supporting body text

// Build an lv_color_t from a 0xRRGGBB constant.
static inline lv_color_t hex_color(uint32_t hex) {
    return lv_color_make((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

static inline void obj_set_hidden(lv_obj_t* obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
