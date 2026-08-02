// Shared screen-layout geometry for the OpenSprinkler panel (lib/ui).
//
// Portable LVGL-only header (no Arduino, no TFT_eSPI, no network) extracted
// verbatim from src/main.cpp so the firmware and the host `sim` build the exact
// same widget geometry. ui_theme.h carries the visual tokens + SCREEN_*/TOP_H;
// this header carries the derived panel/grid/programs geometry the screen
// builders share. All values are pixels, 480x320 landscape.
#pragma once

#include "ui_theme.h"  // SCREEN_W, SCREEN_H, TOP_H

// ---- Core content layout --------------------------------------------------
static constexpr int GRID_H   = 112;  // 108->112: container=92px fits 2xpill_h(40)+gap(6)+pad(4)=90px
static constexpr int ACTION_H = 52;
static constexpr int RIGHT_W  = 190;
static constexpr int LEFT_W   = SCREEN_W - RIGHT_W;  // 290 px
// Content area between top bar and grid.
static constexpr int CONTENT_Y = TOP_H + 1;
static constexpr int CONTENT_H = SCREEN_H - CONTENT_Y - GRID_H;
// Panels stop ACTION_H above the grid.
static constexpr int PANEL_H   = CONTENT_H - ACTION_H;
static constexpr int ACTION_Y  = CONTENT_Y + PANEL_H;
static constexpr int GRID_Y    = ACTION_Y + ACTION_H;

// Full-height layout, used when the station grid is hidden (programs list and
// the program-run queue list). These surfaces have their own widgets, so they
// simply build at the taller geometry - no runtime resize needed.
static constexpr int FULL_BOTTOM   = SCREEN_H - 4;                  // 316
// The program-run screen shares the manual status panel + action row, so its
// left column stops at the SAME action row (ACTION_Y=156, PANEL_H). Only the
// right queue list keeps the full height down to the bottom edge.
static constexpr int FULL_RIGHT_H  = FULL_BOTTOM - CONTENT_Y;       // right queue list to the bottom
static constexpr int PROG_LIST_H   = FULL_BOTTOM - CONTENT_Y;       // programs-list overlay

// Shared navigation-button height for visual continuity across the
// programs-list "Back" button and the pager arrows. (The "Programs" entry
// button on the home screen instead matches the run-time stepper height.)
static constexpr int NAV_BTN_H = 36;
// Programs-list pager dot indicators (shared by build + per-frame re-centring).
static constexpr int PROG_DOT_W = 10, PROG_DOT_H = 10, PROG_DOT_GAP = 8;
static constexpr int PROG_ARROW_W = 46;  // pager arrow button width

// ---- Fixed widget-array capacities ----------------------------------------
static constexpr int MAX_PROG_ROWS  = 4;  // program rows per page
static constexpr int MAX_PROG_PAGES = 6;  // pager dot capacity
static constexpr int MAX_QROWS      = 9;  // visible program-queue rows

// #127 History overlay: compact single-line rows, ~10 per page (vs 4 programs).
// The pager is a numeric "Page N / M" indicator (not dots) so it scales to the
// dozens of pages a busy 30-day log produces.
static constexpr int MAX_HIST_ROWS  = 10;  // history rows per page

// #127 History row content geometry (shared by the build + per-frame update so
// the update can narrow the name column on manual-run rows to fit the tag).
static constexpr int HIST_ICON_X = 10;  // leading kind glyph x
static constexpr int HIST_NAME_X = 36;  // name column x
static constexpr int HIST_RP     = 10;  // right inset from row edge
static constexpr int HIST_WHEN_W = 96;  // timestamp column width
static constexpr int HIST_DUR_W  = 52;  // duration column width
static constexpr int HIST_COL_G  = 6;   // gap between duration + timestamp
static constexpr int HIST_TAG_W  = 96;  // program/context tag column width
static constexpr int HIST_TAG_G  = 6;   // gap between name and tag
static constexpr int HIST_NAME_W =
    SCREEN_W - HIST_NAME_X -
    (HIST_RP + HIST_WHEN_W + HIST_COL_G + HIST_DUR_W + 8);
// Narrowed name width on manual rows (leaves room for the trailing tag), and
// the fixed x where that tag sits (just past the narrowed name).
static constexpr int HIST_NAME_W_TAG = HIST_NAME_W - HIST_TAG_W - HIST_TAG_G;
static constexpr int HIST_TAG_X = HIST_NAME_X + HIST_NAME_W_TAG + HIST_TAG_G;
