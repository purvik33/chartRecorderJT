/* lv_conf.h - LVGL v9 configuration for the paperless recorder
 * Anything not defined here falls back to LVGL defaults. */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_LOG 0

/* Use the C library printf so %f works in lv_label_set_text_fmt */
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

/* Use the C library heap - LVGL's built-in 64K TLSF pool is too small
 * for this UI (charts, long dropdown lists) and hangs when exhausted */
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB

/* Multi-core rendering: 2 software draw threads (Pi 4 has 4 cores;
 * the other two stay free for the UI logic, comm and logger threads) */
#define LV_USE_OS LV_OS_PTHREAD
#define LV_DRAW_SW_DRAW_UNIT_CNT 2

/* Fonts used by the UI */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
/* default font = Montserrat with engineering symbols added; ³ etc.
 * fall back to the built-in Montserrat for the LV_SYMBOL_ icons */
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(font_units_14)
#define LV_FONT_DEFAULT &font_units_14

/* Display drivers: SDL window on PC, DRM + evdev touch on the Pi */
#ifdef RECORDER_SIM
    #define LV_USE_SDL 1
#endif
#ifdef RECORDER_PI
    #define LV_USE_LINUX_DRM 1
    #define LV_USE_EVDEV 1
#endif

#define LV_DEF_REFR_PERIOD 16

/* decode the embedded brand logo (PNG) */
#define LV_USE_LODEPNG 1

#endif /* LV_CONF_H */
