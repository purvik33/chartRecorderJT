/* ui.c - root layout: status bar / content / nav bar, view switching */
#include "ui.h"
#include "data_model.h"
#include "alarm.h"
#include "comm.h"
#include "config.h"
#include "users.h"
#include "export.h"
#include <stdio.h>
#include <time.h>
#include <math.h>

static lv_obj_t *content;
static lv_obj_t *lbl_group;
static lv_obj_t *lbl_clock;
static lv_obj_t *lbl_wifi;
static lv_obj_t *lbl_usb;
static lv_obj_t *eth_ic;
static lv_obj_t *nav_btns[VIEW_COUNT];
static view_id_t cur_view = VIEW_DIGITAL;
static int cur_group = 0;

static const char *view_names[VIEW_COUNT] =
    { "Digital", "Trend", "Polar", "Bar", "Alarm", "Menu" };
static const char *view_icons[VIEW_COUNT] =
    { LV_SYMBOL_LIST, LV_SYMBOL_IMAGE, LV_SYMBOL_REFRESH, LV_SYMBOL_BARS,
      LV_SYMBOL_BELL, LV_SYMBOL_SETTINGS };

/* ---- product themes ---- */
const ui_theme_t ui_themes[UI_THEME_N] = {
    /* Dark (default) */
    { 0x1A1D21, 0x22262B, 0x3A3F45, 0xE8EAED, 0x9AA0A6, 0x5DCAA5,
      0xE24B4A, 0xF09595, 0x3B1518 },
    /* Light */
    { 0xF2F4F7, 0xFFFFFF, 0xC9CFD8, 0x1A1D21, 0x667085, 0x0F9D77,
      0xD92D20, 0xB42318, 0xFEE4E2 },
    /* Ocean blue */
    { 0x0D1B2A, 0x15293E, 0x2C4A63, 0xE6EEF5, 0x8FA6B8, 0x4FC3F7,
      0xE24B4A, 0xF09595, 0x3B1518 },
    /* Amber industrial */
    { 0x23211C, 0x2E2B24, 0x4A443A, 0xF0EDE6, 0xA8A296, 0xF0B429,
      0xE24B4A, 0xF09595, 0x3B1518 },
};
const char *ui_theme_names[UI_THEME_N] =
    { "Dark", "Light", "Ocean blue", "Amber industrial" };

const ui_theme_t *ui_theme(void)
{
    int t = g_cfg.theme;
    if (t < 0 || t >= UI_THEME_N) t = 0;
    return &ui_themes[t];
}

/* white logo on dark backgrounds, red logo on light backgrounds */
static int theme_is_dark(void)
{
    uint32_t bg = ui_theme()->bg;
    int lum = (((bg >> 16) & 0xFF) * 3 + ((bg >> 8) & 0xFF) * 6 +
               (bg & 0xFF)) / 10;
    return lum < 128;
}

const lv_image_dsc_t *ui_bolt_sm(void)
{
    return theme_is_dark() ? &img_bolt_sm_wht : &img_bolt_sm_red;
}

const lv_image_dsc_t *ui_bolt_lg(void)
{
    return theme_is_dark() ? &img_bolt_lg_wht : &img_bolt_lg_red;
}

lv_color_t ui_brand_color(void)
{
    return theme_is_dark() ? lv_color_hex(0xF0F2F5)
                           : lv_color_hex(0xE61E24);
}

/* first 8 entries match the trend palette so multi-colour defaults
 * line up with the trend series colours */
const uint32_t ui_palette[UI_PALETTE_N] = {
    0x5DCAA5, 0x85B7EB, 0xF0997B, 0xED93B1,
    0xFAC775, 0xAFA9EC, 0x97C459, 0xF09595,
    0xE8EAED, 0xEF9F27
};
const char *ui_palette_names =
    "Teal\nBlue\nCoral\nPink\nYellow\nPurple\nGreen\nRed\nWhite\nOrange";

lv_color_t ui_ch_color(int ch)
{
    int idx;
    if (g_cfg.color_mode == 1) {
        idx = g_cfg.single_color;
    } else if (ch >= 0 && ch < CH_TOTAL) {
        idx = g_cfg.ch_color[ch];
    } else {
        idx = 0;
    }
    if (idx < 0 || idx >= UI_PALETTE_N) idx = ch % CH_PER_GROUP;
    return lv_color_hex(ui_palette[idx]);
}

/* graph channel visibility ticks (default: all shown) */
static uint8_t ch_vis[CH_PER_GROUP] = { 1, 1, 1, 1, 1, 1, 1, 1 };

int ui_ch_visible(int pos)
{
    return (pos >= 0 && pos < CH_PER_GROUP) ? ch_vis[pos] : 1;
}

void ui_ch_visible_toggle(int pos)
{
    if (pos >= 0 && pos < CH_PER_GROUP) ch_vis[pos] = !ch_vis[pos];
}

lv_obj_t *ui_content(void) { return content; }
int ui_group(void) { return cur_group; }

/* ---- floating pop-up notification (toast) ------------------------------ */

static void toast_close_cb(lv_timer_t *t)
{
    lv_obj_t *o = (lv_obj_t *)lv_timer_get_user_data(t);
    if (o && lv_obj_is_valid(o)) lv_obj_delete(o);
    lv_timer_delete(t);
}

void ui_toast(const char *msg, int is_error)
{
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(box, is_error ? COL_ALARM : COL_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, is_error ? COL_ALARM : COL_ACCENT, 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_pad_hor(box, 34, 0);
    lv_obj_set_style_pad_ver(box, 30, 0);
    lv_obj_set_style_shadow_width(box, 40, 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_50, 0);
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_style_min_width(box, 420, 0);
    lv_obj_set_style_max_width(box, 700, 0);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_label_create(box);
    lv_label_set_text(ic, is_error ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
    lv_obj_set_style_text_color(ic, is_error ? lv_color_white() : COL_ACCENT, 0);
    lv_obj_set_style_text_font(ic, &font_units_28, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, is_error ? lv_color_white() : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &font_units_28, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 560);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 52, 0);

    lv_obj_align(box, LV_ALIGN_CENTER, 0, 0);

    lv_timer_t *t = lv_timer_create(toast_close_cb, 3500, box);
    lv_timer_set_repeat_count(t, 1);
}

int ui_group_count(void)
{
    if (g_cfg.source == SRC_MODBUS &&
        g_cfg.cards >= 1 && g_cfg.cards <= GROUP_COUNT)
        return g_cfg.cards;
    return GROUP_COUNT;
}

static void update_group_label(void)
{
    lv_label_set_text_fmt(lbl_group, "Group %d  |  CH %d-%d",
                          cur_group + 1,
                          cur_group * CH_PER_GROUP + 1,
                          cur_group * CH_PER_GROUP + CH_PER_GROUP);
}

static void build_view(void)
{
    lv_obj_clean(content);
    /* screens set their own layout/padding; reset what the previous
     * view left behind so views never inherit each other's layout */
    lv_obj_set_layout(content, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_set_style_pad_column(content, 0, 0);

    switch (cur_view) {
    case VIEW_DIGITAL: scr_digital_build(content); break;
    case VIEW_TREND:   scr_trend_build(content);   break;
    case VIEW_POLAR:   scr_polar_build(content);   break;
    case VIEW_BAR:     scr_bar_build(content);     break;
    case VIEW_ALARM:   scr_alarm_build(content);   break;
    case VIEW_MENU:    scr_menu_build(content);    break;
    default: break;
    }
}

static void nav_cb(lv_event_t *e)
{
    view_id_t id = (view_id_t)(intptr_t)lv_event_get_user_data(e);
    if (id == cur_view) return;
    if (cur_view == VIEW_MENU) cfr_logout();  /* 21 CFR: leaving the
                                               * menu ends the session */
    cur_view = id;
    for (int i = 0; i < VIEW_COUNT; i++) {
        lv_obj_set_style_border_width(nav_btns[i], i == (int)id ? 2 : 0, 0);
        lv_obj_set_style_text_color(
            lv_obj_get_child(nav_btns[i], 0),
            i == (int)id ? COL_TEXT : COL_MUTED, 0);
    }
    build_view();
}

static void group_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int cnt = ui_group_count();
    cur_group = (cur_group + dir + cnt) % cnt;
    update_group_label();
    build_view();
}

static lv_obj_t *lbl_alarm;
static lv_obj_t *rs_ic;      /* RS-485 bus link icon (multidrop) */

static void splash_timer_cb(lv_timer_t *t)
{
    lv_obj_delete((lv_obj_t *)lv_timer_get_user_data(t));
}

/* ---- splash: animated pharma production line ----------------------------
 * The environment this recorder is installed in: a mixing reactor with
 * a turning agitator feeding a filling head, vials riding a conveyor
 * under falling drops, and the plant HMI cabinet with live status
 * lamps watching the line. Anim vars are the objects themselves, so
 * deleting the splash removes every animation with it. */

static void spl_anim_x(void *obj, int32_t v)  { lv_obj_set_x(obj, v); }
static void spl_anim_y(void *obj, int32_t v)  { lv_obj_set_y(obj, v); }
static void spl_anim_h(void *obj, int32_t v)  { lv_obj_set_height(obj, v); }
static void spl_anim_rot(void *obj, int32_t v)
{
    lv_obj_set_style_transform_rotation(obj, v, 0);
}
static void spl_anim_opa(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0);
}

static void spl_anim(lv_obj_t *obj, int32_t lo, int32_t hi, uint32_t dur,
                     uint32_t delay, int pingpong, lv_anim_exec_xcb_t cb)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, lo, hi);
    lv_anim_set_duration(&a, dur);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (pingpong) {
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    }
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_start(&a);
}

/* plain faint rectangle, the scene's building block */
static lv_obj_t *spl_rect(lv_obj_t *par, int x, int y, int w, int h,
                          lv_color_t col, lv_opa_t opa, int radius)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* control cabinet: outline, live screen with data lines, blinking
 * status lamps - like the panels flanking the reference scene */
static void spl_cabinet(lv_obj_t *par, int x)
{
    lv_obj_t *cab = lv_obj_create(par);
    lv_obj_set_size(cab, 86, 152);
    lv_obj_set_pos(cab, x, 292);
    lv_obj_set_style_bg_opa(cab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(cab, COL_MUTED, 0);
    lv_obj_set_style_border_opa(cab, 100, 0);
    lv_obj_set_style_border_width(cab, 2, 0);
    lv_obj_set_style_radius(cab, 6, 0);
    lv_obj_set_style_pad_all(cab, 0, 0);
    lv_obj_remove_flag(cab, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *scr = lv_obj_create(cab);
    lv_obj_set_size(scr, 62, 42);
    lv_obj_set_pos(scr, 11, 10);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(scr, COL_MUTED, 0);
    lv_obj_set_style_border_opa(scr, 80, 0);
    lv_obj_set_style_border_width(scr, 1, 0);
    lv_obj_set_style_radius(scr, 3, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* "data" lines on the screen */
    spl_rect(scr, 7, 8,  44, 2, COL_ACCENT, 110, 0);
    spl_rect(scr, 7, 17, 30, 2, COL_ACCENT, 90, 0);
    spl_rect(scr, 7, 26, 38, 2, COL_ACCENT, 100, 0);

    /* two rows of blinking status lamps */
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++) {
            lv_obj_t *led = spl_rect(cab, 15 + c * 22, 68 + r * 22,
                                     11, 11, COL_ACCENT, 60,
                                     LV_RADIUS_CIRCLE);
            spl_anim(led, 40, 220, 450 + (uint32_t)(r * 3 + c) * 90,
                     (uint32_t)(r * 3 + c) * 150, 1, spl_anim_opa);
        }

    /* louvre slots at the bottom */
    spl_rect(cab, 15, 122, 56, 3, COL_MUTED, 80, 0);
    spl_rect(cab, 15, 130, 56, 3, COL_MUTED, 80, 0);
    spl_rect(cab, 15, 138, 56, 3, COL_MUTED, 80, 0);
}

static void splash_scene(lv_obj_t *splash)
{
    /* clean-room floor */
    spl_rect(splash, 36, 446, 728, 2, COL_MUTED, 60, 0);

    /* clean-room lights */
    for (int i = 0; i < 2; i++) {
        int lx = 280 + i * 210;
        spl_rect(splash, lx + 15, 18, 2, 34, COL_MUTED, 70, 0);
        spl_rect(splash, lx, 52, 32, 9, COL_MUTED, 90, 4);
        spl_rect(splash, lx + 8, 63, 16, 3, COL_ACCENT, 120, 2);
    }

    /* plant HMI cabinet - the recorder watching the line */
    spl_cabinet(splash, 668);

    /* ---- mixing reactor with a turning agitator ---- */
    lv_obj_t *ves = lv_obj_create(splash);
    lv_obj_set_size(ves, 104, 128);
    lv_obj_set_pos(ves, 56, 300);
    lv_obj_set_style_bg_opa(ves, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ves, COL_MUTED, 0);
    lv_obj_set_style_border_opa(ves, 100, 0);
    lv_obj_set_style_border_width(ves, 2, 0);
    lv_obj_set_style_radius(ves, 20, 0);
    lv_obj_set_style_pad_all(ves, 5, 0);
    lv_obj_remove_flag(ves, LV_OBJ_FLAG_SCROLLABLE);

    /* batch level breathing while the agitator mixes */
    lv_obj_t *liq = lv_obj_create(ves);
    lv_obj_set_size(liq, 86, 62);
    lv_obj_align(liq, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(liq, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(liq, 50, 0);
    lv_obj_set_style_border_width(liq, 0, 0);
    lv_obj_set_style_radius(liq, 14, 0);
    lv_obj_remove_flag(liq, LV_OBJ_FLAG_SCROLLABLE);
    spl_anim(liq, 56, 74, 1500, 0, 1, spl_anim_h);

    /* drive motor + shaft down into the batch */
    spl_rect(splash, 96, 272, 26, 16, COL_MUTED, 100, 3);
    spl_rect(splash, 106, 288, 4, 56, COL_MUTED, 110, 0);

    /* agitator impeller: 3 swept blades on a canvas, spun smoothly */
    static uint8_t agi_buf[40 * 40 * 4];
    lv_obj_t *agi = lv_canvas_create(ves);
    lv_canvas_set_buffer(agi, agi_buf, 40, 40, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(agi, lv_color_hex(0x000000), LV_OPA_TRANSP);
    {
        lv_layer_t layer;
        lv_canvas_init_layer(agi, &layer);
        lv_draw_arc_dsc_t hub;
        lv_draw_arc_dsc_init(&hub);
        hub.center.x = 20; hub.center.y = 20;
        hub.radius = 5; hub.width = 5;
        hub.start_angle = 0; hub.end_angle = 360;
        hub.color = lv_color_hex(ui_theme()->accent);
        hub.opa = 170;
        lv_draw_arc(&layer, &hub);
        for (int i = 0; i < 3; i++) {
            float dir = (float)(i * 120 + 60) * 3.1415926f / 180.0f;
            lv_draw_arc_dsc_t bl;
            lv_draw_arc_dsc_init(&bl);
            bl.center.x = (int16_t)(20.0f + 7.0f * cosf(dir));
            bl.center.y = (int16_t)(20.0f + 7.0f * sinf(dir));
            bl.radius = 10; bl.width = 4; bl.rounded = 1;
            bl.start_angle = (int16_t)(i * 120 + 150);
            bl.end_angle   = (int16_t)(i * 120 + 265);
            bl.color = lv_color_hex(ui_theme()->accent);
            bl.opa = 170;
            lv_draw_arc(&layer, &bl);
        }
        lv_canvas_finish_layer(agi, &layer);
    }
    lv_obj_set_pos(agi, 27, 62);
    lv_obj_set_style_transform_pivot_x(agi, 20, 0);
    lv_obj_set_style_transform_pivot_y(agi, 20, 0);
    spl_anim(agi, 0, 3600, 1000, 0, 0, spl_anim_rot);

    /* vessel legs */
    spl_rect(splash, 70, 428, 7, 18, COL_MUTED, 90, 0);
    spl_rect(splash, 139, 428, 7, 18, COL_MUTED, 90, 0);

    /* ---- transfer line to the filling head ---- */
    spl_rect(splash, 160, 322, 262, 4, COL_MUTED, 70, 0);
    spl_rect(splash, 418, 322, 4, 24, COL_MUTED, 70, 0);

    /* filling head with nozzle */
    spl_rect(splash, 404, 344, 32, 18, COL_MUTED, 110, 4);
    spl_rect(splash, 415, 362, 10, 9, COL_MUTED, 110, 2);

    /* falling drops into the passing vials */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *drop = spl_rect(splash, 417, 372, 7, 7, COL_ACCENT,
                                  170, LV_RADIUS_CIRCLE);
        spl_anim(drop, 372, 394, 520, (uint32_t)i * 260, 0, spl_anim_y);
    }

    /* ---- conveyor with vials riding through the filler ---- */
    lv_obj_t *belt = lv_obj_create(splash);
    lv_obj_set_size(belt, 416, 13);
    lv_obj_set_pos(belt, 248, 402);
    lv_obj_set_style_bg_color(belt, COL_MUTED, 0);
    lv_obj_set_style_bg_opa(belt, 45, 0);
    lv_obj_set_style_border_color(belt, COL_MUTED, 0);
    lv_obj_set_style_border_opa(belt, 100, 0);
    lv_obj_set_style_border_width(belt, 2, 0);
    lv_obj_set_style_radius(belt, 6, 0);
    lv_obj_remove_flag(belt, LV_OBJ_FLAG_SCROLLABLE);

    spl_rect(splash, 278, 415, 7, 31, COL_MUTED, 80, 0);
    spl_rect(splash, 452, 415, 7, 31, COL_MUTED, 80, 0);
    spl_rect(splash, 622, 415, 7, 31, COL_MUTED, 80, 0);

    for (int i = 0; i < 5; i++) {
        /* vial: neck, outlined body, medicine inside */
        lv_obj_t *vial = lv_obj_create(splash);
        lv_obj_set_size(vial, 18, 34);
        lv_obj_set_pos(vial, 252, 368);
        lv_obj_set_style_bg_opa(vial, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(vial, 0, 0);
        lv_obj_set_style_pad_all(vial, 0, 0);
        lv_obj_remove_flag(vial, LV_OBJ_FLAG_SCROLLABLE);

        spl_rect(vial, 5, 0, 8, 7, COL_MUTED, 110, 2);      /* cap  */
        lv_obj_t *body = lv_obj_create(vial);
        lv_obj_set_size(body, 18, 28);
        lv_obj_set_pos(body, 0, 6);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(body, COL_MUTED, 0);
        lv_obj_set_style_border_opa(body, 130, 0);
        lv_obj_set_style_border_width(body, 2, 0);
        lv_obj_set_style_radius(body, 5, 0);
        lv_obj_set_style_pad_all(body, 0, 0);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        spl_rect(body, 3, 12, 12, 12, COL_ACCENT, 110, 3);  /* dose */

        spl_anim(vial, 252, 628, 3600, (uint32_t)i * 720, 0, spl_anim_x);
    }
}

#ifdef RECORDER_PI
/* WiFi signal from /proc/net/wireless: link quality (0..~70) -> 0..4 bars */
static int wifi_level(void)
{
    FILE *f = fopen("/proc/net/wireless", "r");
    if (!f) return -1;
    char line[160];
    int bars = -1;
    while (fgets(line, sizeof(line), f)) {
        char iface[32]; unsigned status; float link = 0, level = 0;
        if (sscanf(line, " %31[^:]: %x %f %f", iface, &status, &link, &level) >= 4) {
            int q = (int)link;
            bars = q >= 56 ? 4 : q >= 42 ? 3 : q >= 28 ? 2 : q >= 1 ? 1 : 0;
            break;
        }
    }
    fclose(f);
    return bars;
}
#else
static int wifi_level(void) { return 4; }   /* PC sim: full WiFi */
#endif

/* WiFi arcs glyph: brand colour + opacity rising with signal; grey when down */
static void wifi_icon_update(void)
{
    int lvl = wifi_level();          /* -1 = no wifi, 0..4 = signal */
    if (lvl < 0) {
        lv_obj_set_style_text_color(lbl_wifi, COL_MUTED, 0);
        lv_obj_set_style_text_opa(lbl_wifi, LV_OPA_40, 0);
    } else {
        lv_obj_set_style_text_color(lbl_wifi, COL_ACCENT, 0);
        lv_obj_set_style_text_opa(lbl_wifi,
            lvl >= 3 ? LV_OPA_COVER : lvl == 2 ? LV_OPA_70 : LV_OPA_40, 0);
    }
}

#ifdef RECORDER_PI
static int eth_present(void)      /* 1 = eth0 cable has link */
{
    FILE *f = fopen("/sys/class/net/eth0/carrier", "r");
    if (!f) return 0;
    int c = 0;
    if (fscanf(f, "%d", &c) != 1) c = 0;
    fclose(f);
    return c == 1;
}
#else
static int eth_present(void) { return 0; }
#endif

static void refresh_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    lv_label_set_text_fmt(lbl_clock, "%02d-%02d-%02d  %02d:%02d:%02d",
                          tm->tm_mday, tm->tm_mon + 1, tm->tm_year % 100,
                          tm->tm_hour, tm->tm_min, tm->tm_sec);

    int act   = alarm_active_count();
    int unack = alarm_unacked_count();
    if (act > 0 || unack > 0) {
        lv_label_set_text_fmt(lbl_alarm, LV_SYMBOL_BELL " %d",
                              act > 0 ? act : unack);
        lv_obj_set_style_text_color(lbl_alarm, COL_ALARM_TXT, 0);
    } else {
        lv_label_set_text(lbl_alarm, LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(lbl_alarm, COL_MUTED, 0);
    }

    {
        lv_color_t lc = (g_cfg.source == SRC_MODBUS)
            ? (comm_link_ok() ? COL_ACCENT : COL_ALARM_TXT)
            : COL_MUTED;
        uint32_t cc = lv_obj_get_child_count(rs_ic);
        for (uint32_t k = 0; k < cc; k++)
            lv_obj_set_style_bg_color(lv_obj_get_child(rs_ic, k), lc, 0);
    }

    wifi_icon_update();

    { char u[128];
      if (usb_find(u, sizeof u)) lv_obj_remove_flag(lbl_usb, LV_OBJ_FLAG_HIDDEN);
      else                       lv_obj_add_flag(lbl_usb, LV_OBJ_FLAG_HIDDEN); }

    if (eth_present()) lv_obj_remove_flag(eth_ic, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(eth_ic, LV_OBJ_FLAG_HIDDEN);

    switch (cur_view) {
    case VIEW_DIGITAL: scr_digital_refresh(); break;
    case VIEW_TREND:   scr_trend_refresh();   break;
    case VIEW_POLAR:   scr_polar_refresh();   break;
    case VIEW_BAR:     scr_bar_refresh();     break;
    case VIEW_ALARM:   scr_alarm_refresh();   break;
    case VIEW_MENU:    scr_menu_refresh();    break;
    default: break;
    }
}

static lv_obj_t *flat_container(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_root(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_text_color(scr, COL_TEXT, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- status bar -------------------------------------------------- */
    lv_obj_t *bar = flat_container(scr);
    lv_obj_set_size(bar, LV_PCT(100), 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, COL_BORDER, 0);

    /* brand lockup: JETPACE text + bolt emblem, always top-left */
    lv_obj_t *wordmark = lv_label_create(bar);
    lv_label_set_text(wordmark, "JETPACE");
    lv_obj_set_style_text_font(wordmark, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wordmark, ui_brand_color(), 0);
    lv_obj_set_style_text_letter_space(wordmark, 1, 0);
    lv_obj_align(wordmark, LV_ALIGN_LEFT_MID, 10, 1);

    lv_obj_t *logo = lv_image_create(bar);
    lv_image_set_src(logo, ui_bolt_sm());
    lv_obj_align_to(logo, wordmark, LV_ALIGN_OUT_RIGHT_MID, 3, -7);

    lv_obj_t *btn_prev = lv_button_create(bar);
    lv_obj_set_size(btn_prev, 36, 30);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 210, 0);
    lv_obj_set_style_bg_color(btn_prev, COL_BG, 0);
    lv_obj_set_style_border_color(btn_prev, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn_prev, 1, 0);
    lv_obj_set_style_shadow_width(btn_prev, 0, 0);
    lv_obj_add_event_cb(btn_prev, group_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *lp = lv_label_create(btn_prev);
    lv_label_set_text(lp, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lp, COL_TEXT, 0);
    lv_obj_center(lp);

    lbl_group = lv_label_create(bar);
    lv_obj_set_style_text_color(lbl_group, COL_MUTED, 0);
    lv_obj_align(lbl_group, LV_ALIGN_LEFT_MID, 258, 0);

    lv_obj_t *btn_next = lv_button_create(bar);
    lv_obj_set_size(btn_next, 36, 30);
    lv_obj_align(btn_next, LV_ALIGN_LEFT_MID, 430, 0);
    lv_obj_set_style_bg_color(btn_next, COL_BG, 0);
    lv_obj_set_style_border_color(btn_next, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn_next, 1, 0);
    lv_obj_set_style_shadow_width(btn_next, 0, 0);
    lv_obj_add_event_cb(btn_next, group_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *ln = lv_label_create(btn_next);
    lv_label_set_text(ln, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ln, COL_TEXT, 0);
    lv_obj_center(ln);

    lbl_clock = lv_label_create(bar);
    lv_obj_set_style_text_color(lbl_clock, COL_MUTED, 0);
    lv_obj_align(lbl_clock, LV_ALIGN_RIGHT_MID, -12, 0);

    /* RS-485 bus link icon: a multidrop trunk with three nodes */
    rs_ic = lv_obj_create(bar);
    lv_obj_remove_style_all(rs_ic);
    lv_obj_set_size(rs_ic, 22, 14);
    lv_obj_remove_flag(rs_ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rs_ic, LV_ALIGN_RIGHT_MID, -166, 0);
    {
        lv_obj_t *e;
        e = lv_obj_create(rs_ic); lv_obj_remove_style_all(e);   /* trunk */
        lv_obj_set_size(e, 20, 2); lv_obj_set_pos(e, 1, 6);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(e, COL_MUTED, 0);
        int nx[3] = { 0, 9, 18 };                               /* nodes */
        for (int k = 0; k < 3; k++) {
            e = lv_obj_create(rs_ic); lv_obj_remove_style_all(e);
            lv_obj_set_size(e, 4, 4); lv_obj_set_pos(e, nx[k], 5);
            lv_obj_set_style_radius(e, 1, 0);
            lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(e, COL_MUTED, 0);
        }
    }

    lbl_alarm = lv_label_create(bar);
    lv_label_set_text(lbl_alarm, LV_SYMBOL_BELL);
    lv_obj_set_style_text_color(lbl_alarm, COL_MUTED, 0);
    lv_obj_align(lbl_alarm, LV_ALIGN_RIGHT_MID, -230, 0);

    /* WiFi logo (arcs), just left of the alarm bell */
    lbl_wifi = lv_label_create(bar);
    lv_label_set_text(lbl_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(lbl_wifi, COL_MUTED, 0);
    lv_obj_align_to(lbl_wifi, lbl_alarm, LV_ALIGN_OUT_LEFT_MID, -14, 0);

    /* USB drive icon, left of WiFi; shown only when a stick is mounted */
    lbl_usb = lv_label_create(bar);
    lv_label_set_text(lbl_usb, LV_SYMBOL_USB);
    lv_obj_set_style_text_color(lbl_usb, COL_ACCENT, 0);
    lv_obj_align_to(lbl_usb, lbl_wifi, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_flag(lbl_usb, LV_OBJ_FLAG_HIDDEN);

    /* Ethernet (RJ45 plug) icon, left of USB; shown when eth0 has link */
    eth_ic = lv_obj_create(bar);
    lv_obj_remove_style_all(eth_ic);
    lv_obj_set_size(eth_ic, 16, 14);
    lv_obj_remove_flag(eth_ic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(eth_ic, lbl_usb, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    {
        lv_obj_t *e;
        e = lv_obj_create(eth_ic); lv_obj_remove_style_all(e);   /* body */
        lv_obj_set_size(e, 12, 8); lv_obj_set_pos(e, 2, 3);
        lv_obj_set_style_radius(e, 1, 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(e, COL_ACCENT, 0);
        e = lv_obj_create(eth_ic); lv_obj_remove_style_all(e);   /* cable lead */
        lv_obj_set_size(e, 4, 3); lv_obj_set_pos(e, 6, 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(e, COL_ACCENT, 0);
        for (int i = 0; i < 3; i++) {                            /* contact pins */
            e = lv_obj_create(eth_ic); lv_obj_remove_style_all(e);
            lv_obj_set_size(e, 2, 2); lv_obj_set_pos(e, 3 + i * 4, 11);
            lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(e, COL_ACCENT, 0);
        }
    }
    lv_obj_add_flag(eth_ic, LV_OBJ_FLAG_HIDDEN);

    /* ---- content ------------------------------------------------------ */
    content = flat_container(scr);
    lv_obj_set_size(content, LV_PCT(100), 480 - 40 - 56);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(content, COL_BG, 0);

    /* ---- nav bar ------------------------------------------------------ */
    lv_obj_t *nav = flat_container(scr);
    lv_obj_set_size(nav, LV_PCT(100), 56);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(nav, COL_PANEL, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < VIEW_COUNT; i++) {
        lv_obj_t *b = lv_button_create(nav);
        lv_obj_set_size(b, 124, 44);
        lv_obj_set_style_bg_color(b, COL_PANEL, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_color(b, COL_ACCENT, 0);
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(b, i == 0 ? 2 : 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%s  %s", view_icons[i], view_names[i]);
        lv_obj_set_style_text_color(l, i == 0 ? COL_TEXT : COL_MUTED, 0);
        lv_obj_center(l);
        nav_btns[i] = b;
    }

    update_group_label();
    build_view();
}

void ui_reload(void)
{
    lv_obj_clean(lv_screen_active());
    build_root();
}

void ui_init(void)
{
    build_root();
    lv_timer_create(refresh_timer_cb, 500, NULL);

    /* branded startup splash - masks initialization, shows only the
     * customer's brand (never the underlying platform) */
    lv_obj_t *splash = lv_obj_create(lv_layer_top());
    lv_obj_set_size(splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash, COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash, 0, 0);
    lv_obj_set_style_radius(splash, 0, 0);
    lv_obj_remove_flag(splash, LV_OBJ_FLAG_SCROLLABLE);

    splash_scene(splash);   /* animated plant under the brand */

    lv_obj_t *bw = lv_label_create(splash);
    lv_label_set_text(bw, "JETPACE");
    lv_obj_set_style_text_font(bw, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(bw, ui_brand_color(), 0);
    lv_obj_set_style_text_letter_space(bw, 2, 0);
    lv_obj_align(bw, LV_ALIGN_CENTER, -48, -34);

    lv_obj_t *bl = lv_image_create(splash);
    lv_image_set_src(bl, ui_bolt_lg());
    lv_obj_align_to(bl, bw, LV_ALIGN_OUT_RIGHT_MID, 6, -12);

    lv_obj_t *ml = lv_label_create(splash);
    lv_label_set_text(ml, g_cfg.model);
    lv_obj_set_style_text_font(ml, &font_units_16, 0);
    lv_obj_set_style_text_color(ml, COL_ACCENT, 0);
    lv_obj_align(ml, LV_ALIGN_CENTER, 0, 30);

    lv_timer_t *st = lv_timer_create(splash_timer_cb, 3200, splash);
    lv_timer_set_repeat_count(st, 1);
}
