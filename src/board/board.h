// Board HAL — thin, board-agnostic seams for the hardware bits that differ
// between panels (display bring-up, touch, battery, panel reset). Selected at
// compile time by a board build flag (e.g. -D OSP_BOARD_CYD) which picks the
// matching src/board/board_<name>.cpp implementation.
//
// This header intentionally stays free of any concrete driver types (TFT_eSPI,
// Arduino, I2C, ...) so main.cpp depends only on the abstract seams and a new
// board can be added without #ifdef-scattering main.cpp. See issue #120.
#pragma once

#include <cstdint>

// One-time panel reset, run before board_display_init(). No-op on boards whose
// display has no software-driven reset line (e.g. CYD, TFT_RST=-1).
void board_reset_panel();

// Bring up the display controller (init + colour/byte order + rotation) so the
// LVGL flush path and any pre-LVGL boot screens can draw. Does not clear the
// screen — that stays app-side boot chrome.
void board_display_init();

// Ensure the touch layer is ready to poll: apply a saved calibration or, if
// none exists, run the board's interactive calibration and persist it.
// Returns true iff interactive calibration was just performed, so the caller
// can show a "calibration saved" boot notice. No-op returning false on boards
// that need no calibration (e.g. capacitive touch).
bool board_touch_init();

// Poll the touch panel once. Sets `pressed` to whether the panel is currently
// touched; when pressed, writes the touch point to `x`/`y` (in display
// coordinates). Synthetic/bench touch injection is handled by the caller.
void board_touch_read(uint16_t& x, uint16_t& y, bool& pressed);

// Read the battery sense channel and return the (multisampled) millivolts at
// the sense tap. The caller owns sampling cadence and any smoothing/gauge.
int board_battery_read();

// True when the panel is running on external (USB/VBUS) power rather than the
// battery. Drives the top-bar charge glyph. Boards with no charger telemetry
// (e.g. CYD) return false.
bool board_external_power();
