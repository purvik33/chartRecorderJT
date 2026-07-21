/* ui.h - shared UI framework: colors, root layout, navigation, group state */
#ifndef UI_H
#define UI_H

#include "lvgl.h"

/* Product colour themes - selected in Display > Theme, applied on
 * restart. All screens use the COL_* macros, so everything follows. */
typedef struct {
    uint32_t bg, panel, border, text, muted, accent,
             alarm, alarm_txt, alarm_bg;
} ui_theme_t;

#define UI_THEME_N 4
extern const ui_theme_t ui_themes[UI_THEME_N];
extern const char *ui_theme_names[UI_THEME_N];
const ui_theme_t *ui_theme(void);

#define COL_BG        lv_color_hex(ui_theme()->bg)
#define COL_PANEL     lv_color_hex(ui_theme()->panel)
#define COL_BORDER    lv_color_hex(ui_theme()->border)
#define COL_TEXT      lv_color_hex(ui_theme()->text)
#define COL_MUTED     lv_color_hex(ui_theme()->muted)
#define COL_ACCENT    lv_color_hex(ui_theme()->accent)
#define COL_ALARM     lv_color_hex(ui_theme()->alarm)
#define COL_ALARM_TXT lv_color_hex(ui_theme()->alarm_txt)
#define COL_ALARM_BG  lv_color_hex(ui_theme()->alarm_bg)

/* Montserrat with engineering symbols: degree, squared, cubed, micro,
 * ohm (generated from the SVG-source TTF via lv_font_conv) */
LV_FONT_DECLARE(font_units_12);
LV_FONT_DECLARE(font_units_14);
LV_FONT_DECLARE(font_units_16);
LV_FONT_DECLARE(font_units_28);

typedef enum {
    VIEW_DIGITAL = 0,
    VIEW_TREND,
    VIEW_POLAR,
    VIEW_BAR,
    VIEW_ALARM,
    VIEW_MENU,
    VIEW_COUNT
} view_id_t;

void ui_init(void);

/* rebuild the whole UI with the current theme (live theme change) */
void ui_reload(void);

/* Container the active view builds itself into (below status bar, above nav) */
lv_obj_t *ui_content(void);

/* Currently displayed channel group, 0..GROUP_COUNT-1 */
int  ui_group(void);

/* Number of groups the user can page through (fitted cards only) */
int  ui_group_count(void);

/* channel colour palette (Display > Colours menu) */
#define UI_PALETTE_N 10
extern const uint32_t ui_palette[UI_PALETTE_N];
extern const char    *ui_palette_names;   /* \n separated, for dropdowns */

/* display colour of a channel (0..39), honoring single-colour mode */
lv_color_t ui_ch_color(int ch);

/* channel show/hide ticks shared by the trend and polar graphs
 * (per position 1..8 within the group) */
int  ui_ch_visible(int pos);
void ui_ch_visible_toggle(int pos);

/* brand lockup: "JETPACE" drawn as text plus the bolt emblem image.
 * Red on light themes, white on dark themes. */
LV_IMAGE_DECLARE(img_bolt_sm_red);
LV_IMAGE_DECLARE(img_bolt_sm_wht);
LV_IMAGE_DECLARE(img_bolt_lg_red);
LV_IMAGE_DECLARE(img_bolt_lg_wht);
const lv_image_dsc_t *ui_bolt_sm(void);
const lv_image_dsc_t *ui_bolt_lg(void);
lv_color_t ui_brand_color(void);

/* Each view implements: build into parent, and optional 500 ms refresh */
void scr_digital_build(lv_obj_t *parent);
void scr_digital_refresh(void);
void scr_trend_build(lv_obj_t *parent);
void scr_trend_refresh(void);
void scr_polar_build(lv_obj_t *parent);
void scr_polar_refresh(void);
void scr_bar_build(lv_obj_t *parent);
void scr_bar_refresh(void);
void scr_alarm_build(lv_obj_t *parent);
void scr_alarm_refresh(void);
void scr_menu_build(lv_obj_t *parent);
void scr_menu_refresh(void);

#endif
