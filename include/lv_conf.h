/**
 * lv_conf.h — LVGL 9 configuration for the OpenSprinkler panel.
 *
 * Target: ESP32-WROOM-32E (no PSRAM), ST7796U display 480×320.
 * Activated via -D LV_CONF_INCLUDE_SIMPLE=1 in platformio.ini so LVGL
 * locates this file through the compiler's include path (include/ dir).
 *
 * Key constraints vs. the LVGL defaults:
 *   - 16-bit color (RGB565) to match the ST7796U and save RAM.
 *   - System malloc/free (LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB) so the
 *     LVGL heap stays on the FreeRTOS heap; no 64 KB static BSS pool.
 *   - Only the widgets used by the panel UI are enabled.
 *   - No file system, no PNG/JPEG decoder.
 */

#if 1  /* must be 1 to enable this file */

#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef __ASSEMBLY__
#include <stdint.h>
#endif

/* ---- Color ------------------------------------------------------------- */
/** Color depth: 16 = RGB565 (matches ST7796U native format, saves RAM).  */
#define LV_COLOR_DEPTH 16

/* ---- Memory ------------------------------------------------------------ */
/** Delegate to the system allocator (malloc/free) so the LVGL heap does
 *  NOT consume BSS on the no-PSRAM ESP32.  The Arduino heap has ~180 KB
 *  free after the WiFi stack, which is enough for the panel widgets.
 *
 *  LVGL 9.x uses LV_USE_STDLIB_MALLOC (not the LVGL 8.x LV_MEM_CUSTOM flag).
 *  LV_STDLIB_CLIB = 1 routes allocation to the standard malloc/free/realloc,
 *  which on Arduino/FreeRTOS maps to the FreeRTOS heap.  Without this the
 *  default is LV_STDLIB_BUILTIN which silently allocates a 64 KB static BSS
 *  pool — fatal on the 124 KB dram0_0_seg of the no-PSRAM ESP32.            */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB

/* ---- Rendering --------------------------------------------------------- */
/** Refresh period in ms (~30 fps).  The draw callback must call
 *  lv_display_flush_ready() to release the buffer between frames.          */
#define LV_DEF_REFR_PERIOD 33

/* ---- Logging ----------------------------------------------------------- */
/* Diagnostic firmware (-D OSP_DIAG_BUILD) enables LVGL logging at WARN level
 * routed to printf -> Serial (UART0). This makes a failing LVGL assert print
 * its message instead of hanging silently (LVGL asserts log at ERROR, which is
 * >= WARN). Production keeps logging OFF to save flash / avoid overhead. */
#if defined(OSP_DIAG_BUILD)
  #define LV_USE_LOG 1
  #define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
  #define LV_LOG_PRINTF 1
#else
  #define LV_USE_LOG 0   /* disable to save flash; enable for debugging */
#endif

/* ---- Tick -------------------------------------------------------------- */
/** lv_tick_set_cb() lets LVGL call millis() directly (set in setup()).     */
#define LV_TICK_CUSTOM 0   /* we call lv_tick_inc() from a timer instead */

/* ---- Host simulator (sim env only) ------------------------------------- */
/* OSP_SIM is defined ONLY by the native `sim` PlatformIO env. It enables the
 * LVGL SDL driver so the host can open an optional preview window. Everything
 * else above (color depth, fonts, widgets) is shared UNCHANGED with the ESP32
 * firmware, so the sim renders byte-identical Montserrat glyphs. The firmware
 * build never defines OSP_SIM, so LV_USE_SDL stays 0 there.                 */
#if defined(OSP_SIM)
  #define LV_USE_SDL              1
  #define LV_SDL_INCLUDE_PATH     <SDL.h>
  #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
  #define LV_SDL_BUF_COUNT        1
  #define LV_SDL_FULLSCREEN       0
  #define LV_SDL_DIRECT_EXIT      1
  /* lv_snapshot_take() support so the headless path can grab the framebuffer. */
  #define LV_USE_SNAPSHOT         1
#endif

/* ---- Fonts ------------------------------------------------------------- */
/** Built-in Montserrat variants used by the panel UI.  Others disabled to
 *  reduce flash usage (~3–4 KB per unused size).                           */
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1   /* large countdown + station name */

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Uncompressed font bitmaps for speed on a single-core loop.               */
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_PLACEHOLDER 1

/* ---- Widgets (enable only what the panel uses) ------------------------- */
#define LV_USE_LABEL   1
#define LV_USE_BTN     1
#define LV_USE_SWITCH  1
#define LV_USE_OBJ     1

/* ---- Theme ------------------------------------------------------------- */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1      /* dark background */
#define LV_USE_THEME_SIMPLE  0
#define LV_USE_THEME_MONO    0

/* ---- Layout ------------------------------------------------------------ */
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* ---- Input device ------------------------------------------------------ */
#define LV_INDEV_DEF_READ_PERIOD 33  /* ms between touch reads */

/* ---- Animation --------------------------------------------------------- */
#define LV_USE_ANIM 1

/* ---- Misc -------------------------------------------------------------- */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* Disable unused components to save flash.                                 */
#define LV_USE_SPINBOX       0
#define LV_USE_KEYBOARD      0
#define LV_USE_MSGBOX        0
#define LV_USE_SPINNER       0
#define LV_USE_CALENDAR      0
#define LV_USE_CHART         0
#define LV_USE_TABLE         0
#define LV_USE_TABVIEW       0
#define LV_USE_WIN           0
#define LV_USE_MENU          0
#define LV_USE_ARC           0
#define LV_USE_ARCLABEL      0
#define LV_USE_BAR           0
#define LV_USE_CHECKBOX      0
#define LV_USE_DROPDOWN      0
#define LV_USE_IMAGE         0
#define LV_USE_IMG           0
#define LV_USE_IMAGEBUTTON   0
#define LV_USE_LED           0
#define LV_USE_ROLLER        0
#define LV_USE_SLIDER        0
#define LV_USE_TEXTAREA      0
#define LV_USE_LIST          0
#define LV_USE_TILEVIEW      0
#define LV_USE_ANIMIMG       0
#define LV_USE_CANVAS        0
#define LV_USE_SCALE         0
#define LV_USE_SPAN          0
#define LV_USE_LINE          0
#define LV_USE_BUTTONMATRIX  0
#define LV_USE_FS_STDIO      0
#define LV_USE_PNG           0
#define LV_USE_BMP           0
#define LV_USE_TJPGD         0
#define LV_USE_GIF           0
/* Demo/extra features — not needed in firmware */
#define LV_USE_DEMO_WIDGETS         0
#define LV_USE_DEMO_BENCHMARK       0
#define LV_USE_DEMO_STRESS          0
#define LV_USE_DEMO_MUSIC           0
#define LV_USE_DEMO_FLEX_LAYOUT     0
#define LV_USE_DEMO_MULTILANG       0

#endif /* LV_CONF_H */
#endif /* Content enable */
