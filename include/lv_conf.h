/**
 * lv_conf.h — LVGL 9 configuration for the OpenSprinkler panel.
 *
 * Target: ESP32-WROOM-32E (no PSRAM), ST7796U display 480×320.
 * Activated via -D LV_CONF_INCLUDE_SIMPLE=1 in platformio.ini so LVGL
 * locates this file through the compiler's include path (include/ dir).
 *
 * Key constraints vs. the LVGL defaults:
 *   - 16-bit color (RGB565) to match the ST7796U and save RAM.
 *   - 128 KB LVGL heap — keeps headroom for WiFi stack (~80 KB) and app.
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
 *  free after the WiFi stack, which is enough for the panel widgets.      */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc

/* ---- Rendering --------------------------------------------------------- */
/** Refresh period in ms (~30 fps).  The draw callback must call
 *  lv_display_flush_ready() to release the buffer between frames.          */
#define LV_DEF_REFR_PERIOD 33

/* ---- Logging ----------------------------------------------------------- */
#define LV_USE_LOG 0   /* disable to save flash; enable for debugging */

/* ---- Tick -------------------------------------------------------------- */
/** lv_tick_set_cb() lets LVGL call millis() directly (set in setup()).     */
#define LV_TICK_CUSTOM 0   /* we call lv_tick_inc() from a timer instead */

/* ---- Fonts ------------------------------------------------------------- */
/** Built-in Montserrat variants used by the panel UI.  Others disabled to
 *  reduce flash usage (~3–4 KB per unused size).                           */
#define LV_FONT_MONTSERRAT_10 0
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
#define LV_USE_LINE    1
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
