/* scr_menu.c - settings, two-level navigation (no sidebar, no scrollbar
 * on the main list):
 *   Level 1: full-width main menu list (5 large rows)
 *   Level 2: the selected section as its own page with a back button
 * The on-screen keyboard overlays the bottom when a field is focused. */
#include "ui.h"
#include "data_model.h"
#include "config.h"
#include "comm.h"
#include "export.h"
#include "events.h"
#include "modbus_tcp.h"
#include "diag.h"
#include "users.h"
#include "update.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>

/* main menu; Account manager only appears while 21 CFR mode is on */
typedef enum {
    M_CHANNEL = 0, M_LOGGING, M_EXPORT, M_EVENTS, M_DISPLAY, M_NETWORK,
    M_ACCOUNTS, M_CARDSVC, M_ABOUT, M_COUNT
} main_sec_t;

static const char *main_names[M_COUNT] =
    { "Channel setup", "Logging", "Data export", "Event log", "Display",
      "Network", "Account manager", "Factory settings", "About" };
static const char *main_icons[M_COUNT] =
    { LV_SYMBOL_LIST, LV_SYMBOL_SD_CARD, LV_SYMBOL_USB, LV_SYMBOL_FILE,
      LV_SYMBOL_IMAGE, LV_SYMBOL_WIFI, LV_SYMBOL_KEYBOARD,
      LV_SYMBOL_SETTINGS, LV_SYMBOL_HOME };
static int main_map[M_COUNT];   /* visible row -> section id */

/* Display submenu */
typedef enum { D_THEME = 0, D_COLORS, D_COUNT } disp_sec_t;
static const char *disp_names[D_COUNT] = { "Theme", "Channel colours" };
static const char *disp_icons[D_COUNT] = { LV_SYMBOL_IMAGE, LV_SYMBOL_TINT };

/* Factory settings submenu */
typedef enum {
    S_COMM = 0, S_CAL, S_CARDCFG, S_REGS, S_DIAG, S_CFR, S_UPDATE,
    S_RESTART, S_COUNT
} svc_sec_t;

static const char *svc_names[S_COUNT] =
    { "Card communication", "Calibration", "Card configuration",
      "Register access", "Self diagnostic", "21 CFR mode",
      "Software update", "Restart recorder" };
static const char *svc_icons[S_COUNT] =
    { LV_SYMBOL_SHUFFLE, LV_SYMBOL_EDIT, LV_SYMBOL_LIST,
      LV_SYMBOL_EYE_OPEN, LV_SYMBOL_CHARGE, LV_SYMBOL_KEYBOARD,
      LV_SYMBOL_DOWNLOAD, LV_SYMBOL_POWER };

/* Network submenu */
typedef enum { N_ETH = 0, N_WIFI, N_COUNT } net_sec_t;
static const char *net_names[N_COUNT] = { "Ethernet", "Wi-Fi" };
static const char *net_icons[N_COUNT] =
    { LV_SYMBOL_UPLOAD, LV_SYMBOL_WIFI };

static lv_obj_t *root;      /* the view's content container */
static lv_obj_t *panel;     /* level-2 scrollable form area */
static lv_obj_t *kb;        /* keyboard, exists only on level 2 */
static lv_obj_t *pin_dlg;   /* Factory settings password dialog */
static lv_obj_t *rst_dlg;   /* hardware-restart confirmation dialog */
static lv_obj_t *den_dlg;   /* access-denied notice (21 CFR role gate) */

/* Save button lives in the page header (top right). It sits muted
 * until any field changes, then lights up in the accent colour so an
 * unsaved page is impossible to miss. */
static lv_obj_t *page_hdr;
static lv_obj_t *save_btn;
static int       form_loading;   /* suppress dirty during programmatic loads */

static void save_set_dirty(int dirty)
{
    if (!save_btn || !lv_obj_is_valid(save_btn)) return;
    lv_obj_set_style_bg_color(save_btn, dirty ? COL_ACCENT : COL_PANEL, 0);
    lv_obj_set_style_border_width(save_btn, dirty ? 0 : 1, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(save_btn, 0),
                                dirty ? COL_BG : COL_MUTED, 0);
}

static void save_mark_dirty(void)
{
    if (!form_loading) save_set_dirty(1);
}

static void dd_dirty_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    save_mark_dirty();
}

static void save_clean_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    save_set_dirty(0);
}

/* create the page's Save button in the header; cb runs first, then
 * the button drops back to its muted "saved" look */
static lv_obj_t *page_save_button(const char *txt, lv_event_cb_t cb)
{
    save_btn = lv_button_create(page_hdr);
    lv_obj_set_size(save_btn, 150, 42);
    lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(save_btn, COL_PANEL, 0);
    lv_obj_set_style_border_color(save_btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(save_btn, 1, 0);
    lv_obj_set_style_shadow_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(save_btn, save_clean_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(save_btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_obj_center(l);
    return save_btn;
}

static void show_main_list(void);
static void show_svc_list(void);
static void show_disp_list(void);
static void show_net_list(void);

/* form field handles */
static lv_obj_t *dd_itype, *sw_chon, *ta_tag, *ta_unit, *ta_lo, *ta_hi,
                *ta_ahi, *ta_alo, *ta_uzero, *ta_uspan, *lbl_chres;
static lv_obj_t *btn_cardtab[GROUP_COUNT], *btn_chtab[CH_PER_GROUP];
static lv_obj_t *row_uzero, *row_uspan;
static int sel_card, sel_ch;   /* channel setup tab selection */
static lv_obj_t *dd_src, *ta_port, *dd_baud, *dd_cards, *ta_slave,
                *ta_regbase, *dd_func, *dd_fmt, *dd_order, *lbl_commres;
static lv_obj_t *dd_interval;
static lv_obj_t *lbl_usb, *lbl_result;
/* calibration / service form */
static lv_obj_t *dd_scard;
/* card configuration form */
static lv_obj_t *dd_cfg_card, *dd_cfgtype[8], *lbl_cfgval[8], *lbl_cfgres;
/* calibration capture buttons (disabled until a session starts) */
static lv_obj_t *btn_cap_lo, *btn_cap_hi;
static lv_obj_t *dd_caltype, *ta_clo, *ta_chi, *lbl_calstat, *lbl_calcount;
static int cal_active;   /* a calibration session is running */
static int cal_slave;    /* slave of the card being calibrated */
static lv_obj_t *dd_rfc, *ta_raddr, *dd_rcnt, *lbl_rres;
static lv_obj_t *ta_waddr, *ta_wval, *sw_arm, *lbl_wres;

static const int baud_vals[]     = { 9600, 19200, 38400, 57600, 115200 };
/* store intervals: 1 minute minimum, samples land on clock boundaries */
static const int interval_vals[] = { 60, 300, 600, 900, 1800, 3600 };

/* ---- keyboard ---------------------------------------------------------- */

/* typing preview bar shown above the keyboard (mobile style) */
static lv_obj_t *kb_prev, *kb_prev_lbl;
static char kb_oldtxt[80];

static void kb_preview_update(lv_obj_t *ta)
{
    if (!kb_prev_lbl) return;
    const char *cur = lv_textarea_get_text(ta);
    if (cur[0]) {
        lv_label_set_text(kb_prev_lbl, cur);
        lv_obj_set_style_text_color(kb_prev_lbl, COL_TEXT, 0);
    } else if (kb_oldtxt[0]) {
        lv_label_set_text_fmt(kb_prev_lbl, "%s", kb_oldtxt);
        lv_obj_set_style_text_color(kb_prev_lbl, COL_MUTED, 0);
    } else {
        lv_label_set_text(kb_prev_lbl, "");
    }
}

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        /* replace-on-edit: remember the old value and start empty */
        snprintf(kb_oldtxt, sizeof(kb_oldtxt), "%s",
                 lv_textarea_get_text(ta));
        lv_textarea_set_text(ta, "");

        lv_keyboard_set_textarea(kb, ta);
        intptr_t kind = (intptr_t)lv_obj_get_user_data(ta);
        lv_keyboard_set_mode(kb, kind == 2 ? LV_KEYBOARD_MODE_USER_1
                               : kind == 1 ? LV_KEYBOARD_MODE_NUMBER
                                           : LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(kb);
        if (kb_prev) {
            lv_obj_remove_flag(kb_prev, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(kb_prev);
        }
        kb_preview_update(ta);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        if (kb_prev && !lv_obj_has_flag(kb_prev, LV_OBJ_FLAG_HIDDEN))
            kb_preview_update(ta);
        save_mark_dirty();
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY ||
               code == LV_EVENT_CANCEL) {
        /* cancelled or left empty: restore the previous value */
        if (code == LV_EVENT_CANCEL ||
            lv_textarea_get_text(ta)[0] == '\0')
            lv_textarea_set_text(ta, kb_oldtxt);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        if (kb_prev) lv_obj_add_flag(kb_prev, LV_OBJ_FLAG_HIDDEN);
    }
}

/* mark a textarea as numeric: number keypad + digits only */
static const char NUM_CHARS[] = "0123456789.-";
static void ta_numeric(lv_obj_t *ta)
{
    lv_obj_set_user_data(ta, (void *)1);
    lv_textarea_set_accepted_chars(ta, NUM_CHARS);
}

/* engineering-unit keyboard: letters plus degree/squared/cubed/micro/
 * ohm and the common unit punctuation */
static const char *unit_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    LV_SYMBOL_BACKSPACE, "\n",
    "m", "k", "g", "h", "s", "c", "b", "p", "a", "r", "\xC2\xB0", "\n",
    "L", "N", "W", "V", "A", "C", "F", "P", "\xC2\xB2", "\xC2\xB3", "\n",
    "%", "/", "-", ".", "\xC2\xB5", "\xCE\xA9", "l", "t", "n",
    LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t unit_ctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2
};

/* mark a textarea as a unit field: symbol keyboard */
static void ta_units(lv_obj_t *ta)
{
    lv_obj_set_user_data(ta, (void *)2);
}

/* ---- shared form widgets ------------------------------------------------ */

static lv_obj_t *form_row(const char *label)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, LV_PCT(100), 50);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, COL_BORDER, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
    return row;
}

static lv_obj_t *form_ta(lv_obj_t *row, const char *init)
{
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, 280, 42);
    lv_obj_align(ta, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ta, COL_BG, 0);
    lv_obj_set_style_text_color(ta, COL_TEXT, 0);
    lv_obj_set_style_border_color(ta, COL_BORDER, 0);
    lv_obj_set_style_radius(ta, 6, 0);
    if (init) lv_textarea_set_text(ta, init);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_ALL, NULL);
    return ta;
}

static lv_obj_t *form_dd(lv_obj_t *row, const char *opts, int sel)
{
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, opts);
    lv_obj_set_size(dd, 280, 42);
    lv_obj_align(dd, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(dd, COL_BG, 0);
    lv_obj_set_style_text_color(dd, COL_TEXT, 0);
    lv_obj_set_style_border_color(dd, COL_BORDER, 0);
    lv_obj_set_style_radius(dd, 6, 0);
    if (sel >= 0) lv_dropdown_set_selected(dd, (uint32_t)sel);

    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(list, COL_PANEL, 0);
    lv_obj_set_style_text_color(list, COL_TEXT, 0);
    lv_obj_set_style_border_color(list, COL_BORDER, 0);
    lv_obj_set_style_max_height(list, 220, 0);
    lv_obj_add_event_cb(dd, dd_dirty_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *action_button(const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_set_size(btn, 220, 46);
    lv_obj_set_style_bg_color(btn, COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, COL_BG, 0);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_center(l);
    return btn;
}

static void ta_set_float(lv_obj_t *ta, float v)
{
    char b[24];
    snprintf(b, sizeof(b), "%g", (double)v);
    lv_textarea_set_text(ta, b);
}

/* ---- section forms ------------------------------------------------------ */

/* datasheet default range/unit per input type code */
static void itype_defaults(int t, float *lo, float *hi, const char **unit)
{
    *unit = "\xC2\xB0""C";   /* degree symbol */
    switch (t) {
    case 1:  *lo = -200; *hi = 1000; break;
    case 2:  *lo = -200; *hi = 1200; break;
    case 3:  *lo = -200; *hi = 1350; break;
    case 4:  *lo = -200; *hi = 400;  break;
    case 5:  *lo =  450; *hi = 1800; break;
    case 6:
    case 7:  *lo =    0; *hi = 1750; break;
    case 8:  *lo = -200; *hi = 1300; break;
    case 9:  *lo = -200; *hi = 850;  break;
    case 10: *lo = -210; *hi = 210;  break;
    case 11: *lo =  -80; *hi = 210;  break;
    case 12: *lo = 0;  *hi = 20;   *unit = "mA"; break;
    case 13: *lo = 4;  *hi = 20;   *unit = "mA"; break;
    case 14: *lo = 0;  *hi = 2000; *unit = "Ohm"; break;
    case 15: *lo = -10; *hi = 50;  *unit = "mV"; break;
    case 16: *lo = 0;  *hi = 100;  *unit = "mV"; break;
    case 17: *lo = 0;  *hi = 250;  *unit = "mV"; break;
    case 18: *lo = 0;  *hi = 5;    *unit = "V";  break;
    case 19: *lo = 0;  *hi = 10;   *unit = "V";  break;
    default: *lo = 0;  *hi = 100;  *unit = "";   break;
    }
}

/* selected input type code: dropdown lists types 1..19 (no Skip);
 * the ON/OFF switch makes it 0 (skip) when off */
static int chform_type(void)
{
    if (!lv_obj_has_state(sw_chon, LV_STATE_CHECKED)) return 0;
    return (int)lv_dropdown_get_selected(dd_itype) + 1;
}

/* counts scaling only applies to linear inputs - hide for RTD/TC */
static void chform_update_visibility(void)
{
    int t = (int)lv_dropdown_get_selected(dd_itype) + 1;
    if (t >= 12 && t <= 19) {
        lv_obj_remove_flag(row_uzero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(row_uspan, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(row_uzero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(row_uspan, LV_OBJ_FLAG_HIDDEN);
    }
}

static void channel_form_load(int ch)
{
    form_loading = 1;
    data_lock();
    channel_t *c = &g_ch[ch];
    lv_textarea_set_text(ta_tag,  c->tag);
    lv_textarea_set_text(ta_unit, c->unit);
    ta_set_float(ta_lo,  c->lo);
    ta_set_float(ta_hi,  c->hi);
    ta_set_float(ta_ahi, c->alm_hi);
    ta_set_float(ta_alo, c->alm_lo);
    data_unlock();

    /* current input type and scaling come from the card itself */
    lv_label_set_text(lbl_chres, "");
    if (g_cfg.source == SRC_MODBUS && ch < g_cfg.cards * CH_PER_GROUP) {
        uint16_t ty, uz, us;
        int card = ch / CH_PER_GROUP;
        int cch  = ch % CH_PER_GROUP;
        int slave = g_cfg.slave_base + card;
        if (mb_service_read(slave, 3, REG_TYPE + cch, 1, &ty) == 0 &&
            ty < 20) {
            if (ty == 0) {
                lv_obj_remove_state(sw_chon, LV_STATE_CHECKED);
            } else {
                lv_obj_add_state(sw_chon, LV_STATE_CHECKED);
                lv_dropdown_set_selected(dd_itype, ty - 1);
            }
        } else {
            lv_label_set_text(lbl_chres, "Card not responding -"
                                         " input type not read");
            lv_obj_set_style_text_color(lbl_chres, COL_ALARM_TXT, 0);
        }
        char b[16];
        if (mb_service_read(slave, 3, REG_USER_ZERO + cch, 1, &uz) == 0) {
            snprintf(b, sizeof(b), "%d", (int16_t)uz);
            lv_textarea_set_text(ta_uzero, b);
        }
        if (mb_service_read(slave, 3, REG_USER_SPAN + cch, 1, &us) == 0) {
            snprintf(b, sizeof(b), "%d", (int16_t)us);
            lv_textarea_set_text(ta_uspan, b);
        }
    }
    chform_update_visibility();
    form_loading = 0;
    save_set_dirty(0);    /* freshly loaded channel = nothing unsaved */
}

static void itype_change_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int t = (int)lv_dropdown_get_selected(dd_itype) + 1;
    float lo, hi;
    const char *unit;
    itype_defaults(t, &lo, &hi, &unit);
    ta_set_float(ta_lo, lo);
    ta_set_float(ta_hi, hi);
    lv_textarea_set_text(ta_unit, unit);
    chform_update_visibility();
}

static void ch_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int ch = sel_card * CH_PER_GROUP + sel_ch;
    data_lock();
    channel_t *c = &g_ch[ch];
    strncpy(c->tag,  lv_textarea_get_text(ta_tag),  sizeof(c->tag) - 1);
    strncpy(c->unit, lv_textarea_get_text(ta_unit), sizeof(c->unit) - 1);
    c->lo     = (float)atof(lv_textarea_get_text(ta_lo));
    c->hi     = (float)atof(lv_textarea_get_text(ta_hi));
    c->alm_hi = (float)atof(lv_textarea_get_text(ta_ahi));
    c->alm_lo = (float)atof(lv_textarea_get_text(ta_alo));
    data_unlock();
    config_save();
    event_log("CONFIG", "CH%d setup saved (%s)", ch + 1,
              chform_type() == 0 ? "OFF"
                                 : mb_type_name((unsigned)chform_type()));

    /* write input type (and scaling for linear types) to the AI card */
    if (g_cfg.source == SRC_MODBUS && ch < g_cfg.cards * CH_PER_GROUP) {
        int card  = ch / CH_PER_GROUP;
        int cch   = ch % CH_PER_GROUP;
        int slave = g_cfg.slave_base + card;
        int ty    = chform_type();   /* 0 when channel switched OFF */

        int ok = (mb_service_write(slave, REG_TYPE + cch,
                                   (uint16_t)ty) == 0);

        /* user zero/span scale the card's output counts - only
         * meaningful for linear inputs (mA / V / mV / ohms) */
        if (ok && ty >= 12 && ty <= 19) {
            int uz = atoi(lv_textarea_get_text(ta_uzero));
            int us = atoi(lv_textarea_get_text(ta_uspan));
            ok = (mb_service_write(slave, REG_USER_ZERO + cch,
                                   (uint16_t)(int16_t)uz) == 0)
              && (mb_service_write(slave, REG_USER_SPAN + cch,
                                   (uint16_t)(int16_t)us) == 0);
        }

        if (ok) {
            comm_refresh_types(card);
            lv_label_set_text_fmt(lbl_chres,
                ty == 0 ? "Saved - card CH%d switched OFF%s"
                        : "Saved - card CH%d set to %s%s (verified)",
                cch + 1,
                ty == 0 ? "" : mb_type_name((unsigned)ty),
                (ty >= 12 && ty <= 19) ? " with zero/span" : "");
            lv_obj_set_style_text_color(lbl_chres, COL_ACCENT, 0);
        } else {
            lv_label_set_text(lbl_chres,
                "Saved on recorder, but card write FAILED");
            lv_obj_set_style_text_color(lbl_chres, COL_ALARM_TXT, 0);
        }
    } else {
        lv_label_set_text(lbl_chres, "Saved");
        lv_obj_set_style_text_color(lbl_chres, COL_ACCENT, 0);
    }
}

/* number of card tabs to show: only fitted cards */
static int chform_cards(void)
{
    if (g_cfg.source == SRC_MODBUS &&
        g_cfg.cards >= 1 && g_cfg.cards <= GROUP_COUNT)
        return g_cfg.cards;
    return GROUP_COUNT;
}

static void chform_tabs_update(void)
{
    int cards = chform_cards();
    for (int i = 0; i < cards; i++) {
        if (btn_cardtab[i] == NULL) continue;
        bool act = (i == sel_card);
        lv_obj_set_style_bg_color(btn_cardtab[i],
                                  act ? COL_BG : COL_PANEL, 0);
        lv_obj_set_style_border_width(btn_cardtab[i], act ? 2 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn_cardtab[i], 0),
                                    act ? COL_TEXT : COL_MUTED, 0);
    }
    for (int i = 0; i < CH_PER_GROUP; i++) {
        bool act = (i == sel_ch);
        lv_obj_set_style_bg_color(btn_chtab[i],
                                  act ? COL_BG : COL_PANEL, 0);
        lv_obj_set_style_border_width(btn_chtab[i], act ? 2 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn_chtab[i], 0),
                                    act ? COL_TEXT : COL_MUTED, 0);
    }
}

static void cardtab_cb(lv_event_t *e)
{
    sel_card = (int)(intptr_t)lv_event_get_user_data(e);
    chform_tabs_update();
    channel_form_load(sel_card * CH_PER_GROUP + sel_ch);
}

static void chtab_cb(lv_event_t *e)
{
    sel_ch = (int)(intptr_t)lv_event_get_user_data(e);
    chform_tabs_update();
    channel_form_load(sel_card * CH_PER_GROUP + sel_ch);
}

static lv_obj_t *tab_button(lv_obj_t *bar, const char *txt,
                            lv_event_cb_t cb, int idx)
{
    lv_obj_t *b = lv_button_create(bar);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, LV_PCT(100));
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_border_color(b, COL_ACCENT, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *tab_bar(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), h);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_style_bg_color(bar, COL_PANEL, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 2, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static void build_channel_form(void)
{
    /* fixed page: tab bars on top, scrollable form below */
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 0, 0);

    int cards = chform_cards();
    if (sel_card >= cards) sel_card = 0;
    int y = 0;

    /* card tabs - hidden when only one card is fitted */
    memset(btn_cardtab, 0, sizeof(btn_cardtab));
    if (cards > 1) {
        lv_obj_t *cbar = tab_bar(panel, y, 40);
        for (int i = 0; i < cards; i++) {
            char t[10];
            snprintf(t, sizeof(t), "Card %d", i + 1);
            btn_cardtab[i] = tab_button(cbar, t, cardtab_cb, i);
        }
        y += 42;
    }

    /* channel tabs CH1..CH8 */
    lv_obj_t *hbar = tab_bar(panel, y, 40);
    for (int i = 0; i < CH_PER_GROUP; i++) {
        char t[6];
        snprintf(t, sizeof(t), "CH%d", i + 1);
        btn_chtab[i] = tab_button(hbar, t, chtab_cb, i);
    }
    y += 44;

    /* scrollable form area below the tabs */
    lv_obj_t *page = panel;
    lv_obj_t *form = lv_obj_create(page);
    lv_obj_set_size(form, LV_PCT(100), (480 - 40 - 56 - 52) - y);
    lv_obj_set_pos(form, 0, y);
    lv_obj_set_style_bg_color(form, COL_BG, 0);
    lv_obj_set_style_border_width(form, 0, 0);
    lv_obj_set_style_pad_hor(form, 20, 0);
    lv_obj_set_style_pad_ver(form, 8, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(form, 6, 0);

    panel = form;   /* form_row helpers build into the form area */

    char topts[300];
    int tlen = 0;
    topts[0] = 0;
    for (unsigned t = 1; t < 20 && tlen < (int)sizeof(topts) - 1; t++)
        tlen += snprintf(topts + tlen, sizeof(topts) - (size_t)tlen, "%s%s",
                         mb_type_name(t), t < 19 ? "\n" : "");

    /* input type + ON/OFF switch share one row */
    lv_obj_t *trow = form_row("Input type");
    dd_itype = lv_dropdown_create(trow);
    lv_dropdown_set_options(dd_itype, topts);
    lv_dropdown_set_selected(dd_itype, 8);   /* Pt-100 */
    lv_obj_set_size(dd_itype, 190, 42);
    lv_obj_align(dd_itype, LV_ALIGN_RIGHT_MID, -90, 0);
    lv_obj_set_style_bg_color(dd_itype, COL_BG, 0);
    lv_obj_set_style_text_color(dd_itype, COL_TEXT, 0);
    lv_obj_set_style_border_color(dd_itype, COL_BORDER, 0);
    lv_obj_t *il = lv_dropdown_get_list(dd_itype);
    lv_obj_set_style_bg_color(il, COL_PANEL, 0);
    lv_obj_set_style_text_color(il, COL_TEXT, 0);
    lv_obj_set_style_max_height(il, 200, 0);
    lv_obj_add_event_cb(dd_itype, itype_change_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *swl = lv_label_create(trow);
    lv_label_set_text(swl, "ON");
    lv_obj_set_style_text_font(swl, &font_units_12, 0);
    lv_obj_set_style_text_color(swl, COL_MUTED, 0);
    lv_obj_align(swl, LV_ALIGN_RIGHT_MID, -58, 0);

    sw_chon = lv_switch_create(trow);
    lv_obj_set_size(sw_chon, 50, 26);
    lv_obj_align(sw_chon, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_state(sw_chon, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw_chon, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_chon, COL_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw_chon, dd_dirty_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);

    ta_tag  = form_ta(form_row("Tag"), NULL);
    ta_unit = form_ta(form_row("Unit"), NULL);
    ta_units(ta_unit);
    ta_lo   = form_ta(form_row("Range low"), NULL);
    ta_hi   = form_ta(form_row("Range high"), NULL);
    ta_ahi  = form_ta(form_row("Alarm high"), NULL);
    ta_alo  = form_ta(form_row("Alarm low"), NULL);
    ta_numeric(ta_lo);
    ta_numeric(ta_hi);
    ta_numeric(ta_ahi);
    ta_numeric(ta_alo);

    /* card output scaling rows - shown only for mA/V/mV/ohm types */
    row_uzero = form_row("Card zero (counts)");
    ta_uzero  = form_ta(row_uzero, NULL);
    row_uspan = form_row("Card span (counts)");
    ta_uspan  = form_ta(row_uspan, NULL);
    ta_numeric(ta_uzero);
    ta_numeric(ta_uspan);

    page_save_button(LV_SYMBOL_SAVE "  Save channel", ch_save_cb);

    lbl_chres = lv_label_create(panel);
    lv_label_set_text(lbl_chres, "Saving writes the settings to the"
                                 " AI card (counts range -2000..20000)");
    lv_obj_set_style_text_font(lbl_chres, &font_units_12, 0);
    lv_obj_set_style_text_color(lbl_chres, COL_MUTED, 0);

    panel = page;   /* restore for the page scaffolding */

    chform_tabs_update();
    channel_form_load(sel_card * CH_PER_GROUP + sel_ch);
}

static void comm_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int old_baud = g_cfg.baud;
    int new_baud = baud_vals[lv_dropdown_get_selected(dd_baud)];

    /* apply everything except the baud first, so the card programming
     * below runs with the just-entered card count / slave base */
    g_cfg.source     = (int)lv_dropdown_get_selected(dd_src);
    strncpy(g_cfg.port, lv_textarea_get_text(ta_port), sizeof(g_cfg.port) - 1);
    g_cfg.cards      = (int)lv_dropdown_get_selected(dd_cards) + 1;
    g_cfg.slave_base = atoi(lv_textarea_get_text(ta_slave));
    g_cfg.func       = lv_dropdown_get_selected(dd_func) == 0 ? 3 : 4;
    g_cfg.reg_base   = atoi(lv_textarea_get_text(ta_regbase));
    g_cfg.fmt        = (int)lv_dropdown_get_selected(dd_fmt);
    g_cfg.word_order = (int)lv_dropdown_get_selected(dd_order);

    /* baud change: reprogram all fitted cards, then follow with our
     * own port - one action changes the whole bus */
    if (g_cfg.source == SRC_MODBUS && new_baud != old_baud) {
        char rep[160];
        int n = comm_bus_baud_change(new_baud, rep, sizeof(rep));
        if (n >= 0) {
            lv_label_set_text_fmt(lbl_commres,
                                  "Bus now %d baud - %d/%d cards OK   (%s)",
                                  new_baud, n, g_cfg.cards, rep);
            lv_obj_set_style_text_color(lbl_commres,
                                        n == g_cfg.cards ? COL_ACCENT
                                                         : COL_ALARM_TXT, 0);
            event_log("CONFIG", "Bus baud %d -> %d; %d/%d cards verified"
                      " (%s)", old_baud, new_baud, n, g_cfg.cards, rep);
        } else {
            lv_label_set_text_fmt(lbl_commres, "%s", rep);
            lv_obj_set_style_text_color(lbl_commres, COL_ALARM_TXT, 0);
            event_log("CONFIG", "Bus baud change %d -> %d failed: %s",
                      old_baud, new_baud, rep);
        }
    } else if (lbl_commres) {
        lv_label_set_text(lbl_commres, "Settings saved");
        lv_obj_set_style_text_color(lbl_commres, COL_MUTED, 0);
    }

    g_cfg.baud = new_baud;
    config_save();
    event_log("CONFIG", "Communication settings saved (%s; %s; %d baud)",
              g_cfg.source == SRC_MODBUS ? "input cards" : "demo",
              g_cfg.port, g_cfg.baud);
}

static void build_comm_form(void)
{
    int bsel = 0;
    for (int i = 0; i < 5; i++)
        if (baud_vals[i] == g_cfg.baud) bsel = i;

    char b[16];
    dd_src   = form_dd(form_row("Data source"),
                       "Demo mode\nInput cards (RS-485)", g_cfg.source);
    ta_port  = form_ta(form_row("Port"), g_cfg.port);
    dd_baud  = form_dd(form_row("Baud rate"),
                       "9600\n19200\n38400\n57600\n115200", bsel);
    dd_cards = form_dd(form_row("Cards fitted"), "1\n2\n3\n4\n5",
                       g_cfg.cards - 1);
    snprintf(b, sizeof(b), "%d", g_cfg.slave_base);
    ta_slave = form_ta(form_row("Slave id (card 1)"), b);
    ta_numeric(ta_slave);
    dd_func  = form_dd(form_row("Function code"),
                       "FC03 holding regs\nFC04 input regs",
                       g_cfg.func == 4 ? 1 : 0);
    snprintf(b, sizeof(b), "%d", g_cfg.reg_base);
    ta_regbase = form_ta(form_row("Register base"), b);
    ta_numeric(ta_regbase);
    dd_fmt   = form_dd(form_row("Value format"),
                       "Float32 (2 regs)\nInt16 / 10\nInt16 / 100\nInt16 raw",
                       g_cfg.fmt);
    dd_order = form_dd(form_row("Float word order"),
                       "CDAB (word swap)\nABCD", g_cfg.word_order);
    page_save_button(LV_SYMBOL_SAVE "  Save", comm_save_cb);

    lbl_commres = lv_label_create(panel);
    lv_label_set_text(lbl_commres, "");
    lv_obj_set_style_text_color(lbl_commres, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_commres, &font_units_14, 0);
    lv_obj_set_width(lbl_commres, LV_PCT(100));
    lv_label_set_long_mode(lbl_commres, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note, "Baud rate changes are written to all fitted"
                            " cards and applied immediately."
                            " Restart the recorder for port / source"
                            " changes.");
    lv_obj_set_style_text_color(note, COL_MUTED, 0);
    lv_obj_set_style_text_font(note, &font_units_12, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ---- Network / Modbus TCP page ---------------------------------------- */

static lv_obj_t *dd_dhcp, *ta_ip, *ta_mask, *ta_gw, *ta_dns,
                *dd_tcpen, *ta_tcpport, *ta_tcpunit, *lbl_netres,
                *dd_weben, *ta_webport;

static void net_status_update(void)
{
    char ip[24];
    net_current_ip(ip, sizeof(ip));
    lv_label_set_text_fmt(lbl_netres,
        "Current address: %s   |   Modbus TCP: %s, port %d, unit %d"
        "\nAI cards direct on unit id 11..%d   |   Dashboard: %s",
        ip, g_cfg.tcp_enable ? "ON" : "OFF", g_cfg.tcp_port,
        g_cfg.tcp_unit, 10 + g_cfg.cards,
        g_cfg.web_enable ? "" : "OFF");
    if (g_cfg.web_enable) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s:%d", ip, g_cfg.web_port);
        lv_label_ins_text(lbl_netres, LV_LABEL_POS_LAST, url);
    }
}

static void net_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    g_cfg.net_dhcp = lv_dropdown_get_selected(dd_dhcp) == 0 ? 1 : 0;
    strncpy(g_cfg.net_ip,   lv_textarea_get_text(ta_ip),
            sizeof(g_cfg.net_ip) - 1);
    strncpy(g_cfg.net_mask, lv_textarea_get_text(ta_mask),
            sizeof(g_cfg.net_mask) - 1);
    strncpy(g_cfg.net_gw,   lv_textarea_get_text(ta_gw),
            sizeof(g_cfg.net_gw) - 1);
    strncpy(g_cfg.net_dns,  lv_textarea_get_text(ta_dns),
            sizeof(g_cfg.net_dns) - 1);
    g_cfg.tcp_enable = lv_dropdown_get_selected(dd_tcpen) == 0 ? 1 : 0;

    int port = atoi(lv_textarea_get_text(ta_tcpport));
    if (port < 1 || port > 65535) port = 502;
    g_cfg.tcp_port = port;

    int unit = atoi(lv_textarea_get_text(ta_tcpunit));
    if (unit < 1 || unit > 247 || (unit >= 11 && unit <= 15)) unit = 1;
    g_cfg.tcp_unit = unit;

    g_cfg.web_enable = lv_dropdown_get_selected(dd_weben) == 0 ? 1 : 0;
    int wport = atoi(lv_textarea_get_text(ta_webport));
    if (wport < 1 || wport > 65535) wport = 8080;
    g_cfg.web_port = wport;

    config_save();
    net_apply();     /* Pi: nmcli; sim: stored only (logged either way) */
    event_log("CONFIG", "Network saved (%s; Modbus TCP %s port %d)",
              g_cfg.net_dhcp ? "DHCP" : g_cfg.net_ip,
              g_cfg.tcp_enable ? "on" : "off", g_cfg.tcp_port);
    net_status_update();
}

static void build_network_form(void)
{
    char b[16];
    dd_dhcp = form_dd(form_row("Address mode"),
                      "DHCP (automatic)\nStatic", g_cfg.net_dhcp ? 0 : 1);
    ta_ip   = form_ta(form_row("IP address"), g_cfg.net_ip);
    ta_numeric(ta_ip);
    ta_mask = form_ta(form_row("Subnet mask"), g_cfg.net_mask);
    ta_numeric(ta_mask);
    ta_gw   = form_ta(form_row("Gateway"), g_cfg.net_gw);
    ta_numeric(ta_gw);
    ta_dns  = form_ta(form_row("DNS server"), g_cfg.net_dns);
    ta_numeric(ta_dns);

    dd_tcpen = form_dd(form_row("Modbus TCP server"),
                       "Enabled\nDisabled", g_cfg.tcp_enable ? 0 : 1);
    snprintf(b, sizeof(b), "%d", g_cfg.tcp_port);
    ta_tcpport = form_ta(form_row("Modbus TCP port"), b);
    ta_numeric(ta_tcpport);
    snprintf(b, sizeof(b), "%d", g_cfg.tcp_unit);
    ta_tcpunit = form_ta(form_row("Recorder unit id"), b);
    ta_numeric(ta_tcpunit);

    dd_weben = form_dd(form_row("Web dashboard"),
                       "Enabled\nDisabled", g_cfg.web_enable ? 0 : 1);
    snprintf(b, sizeof(b), "%d", g_cfg.web_port);
    ta_webport = form_ta(form_row("Web port"), b);
    ta_numeric(ta_webport);

    page_save_button(LV_SYMBOL_SAVE "  Save", net_save_cb);

    lbl_netres = lv_label_create(panel);
    lv_obj_set_style_text_color(lbl_netres, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_netres, &font_units_12, 0);
    lv_obj_set_width(lbl_netres, LV_PCT(100));
    lv_label_set_long_mode(lbl_netres, LV_LABEL_LONG_WRAP);
    net_status_update();

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note,
        "Static IP fields are used when Address mode is Static."
        " The same port carries internet access when the gateway and"
        " DNS are set. Writes from SCADA to card parameters (unit 11+)"
        " are forwarded to the cards over RS-485 automatically.");
    lv_obj_set_style_text_color(note, COL_MUTED, 0);
    lv_obj_set_style_text_font(note, &font_units_12, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ---- Wi-Fi page -------------------------------------------------------- */

static lv_obj_t *dd_wifien, *ta_ssid, *ta_wpass, *dd_wdhcp,
                *ta_wip, *ta_wmask, *ta_wgw, *lbl_wifires;
static lv_obj_t *dd_ssids, *btn_scan, *lbl_scan;

/* background SSID scan (netsh / nmcli take a few seconds) */
static volatile int wscan_busy;
static char         wscan_res[16][33];
static int          wscan_n;
static lv_timer_t  *wscan_timer;

static void *wscan_thread(void *a)
{
    LV_UNUSED(a);
    wscan_n = wifi_scan(wscan_res, 16);
    wscan_busy = 0;
    return NULL;
}

static void wscan_poll(lv_timer_t *t)
{
    if (!dd_ssids || !lv_obj_is_valid(dd_ssids)) {
        lv_timer_delete(t);
        wscan_timer = NULL;
        return;
    }
    if (wscan_busy) return;
    lv_timer_delete(t);
    wscan_timer = NULL;

    if (wscan_n == 0) {
        lv_dropdown_set_options(dd_ssids, "No networks found");
    } else {
        char opts[16 * 34] = "";
        for (int i = 0; i < wscan_n; i++) {
            if (i) strcat(opts, "\n");
            strcat(opts, wscan_res[i]);
        }
        lv_dropdown_set_options(dd_ssids, opts);
        lv_dropdown_open(dd_ssids);
    }
    lv_label_set_text(lbl_scan, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_remove_state(btn_scan, LV_STATE_DISABLED);
}

static void wscan_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (wscan_busy) return;
    wscan_busy = 1;
    lv_label_set_text(lbl_scan, "Scanning...");
    lv_obj_add_state(btn_scan, LV_STATE_DISABLED);
    lv_dropdown_set_options(dd_ssids, "Searching...");
    pthread_t t;
    pthread_create(&t, NULL, wscan_thread, NULL);
    wscan_timer = lv_timer_create(wscan_poll, 150, NULL);
}

static void ssid_pick_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char sel[64];
    lv_dropdown_get_selected_str(dd_ssids, sel, sizeof(sel));
    if (strcmp(sel, "No networks found") && strcmp(sel, "Searching...") &&
        strcmp(sel, "Tap Scan to search"))
        lv_textarea_set_text(ta_ssid, sel);
}

static void wifi_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    g_cfg.wifi_enable = lv_dropdown_get_selected(dd_wifien) == 0 ? 1 : 0;
    strncpy(g_cfg.wifi_ssid, lv_textarea_get_text(ta_ssid),
            sizeof(g_cfg.wifi_ssid) - 1);
    strncpy(g_cfg.wifi_pass, lv_textarea_get_text(ta_wpass),
            sizeof(g_cfg.wifi_pass) - 1);
    g_cfg.wifi_dhcp = lv_dropdown_get_selected(dd_wdhcp) == 0 ? 1 : 0;
    strncpy(g_cfg.wifi_ip,   lv_textarea_get_text(ta_wip),
            sizeof(g_cfg.wifi_ip) - 1);
    strncpy(g_cfg.wifi_mask, lv_textarea_get_text(ta_wmask),
            sizeof(g_cfg.wifi_mask) - 1);
    strncpy(g_cfg.wifi_gw,   lv_textarea_get_text(ta_wgw),
            sizeof(g_cfg.wifi_gw) - 1);
    config_save();
    wifi_apply();
    lv_label_set_text_fmt(lbl_wifires, "Saved - Wi-Fi %s%s%s",
                          g_cfg.wifi_enable ? "enabled" : "disabled",
                          g_cfg.wifi_enable ? ", connecting to " : "",
                          g_cfg.wifi_enable ? g_cfg.wifi_ssid : "");
    lv_obj_set_style_text_color(lbl_wifires, COL_ACCENT, 0);
}

static void build_wifi_form(void)
{
    dd_wifien = form_dd(form_row("Wi-Fi"),
                        "Enabled\nDisabled", g_cfg.wifi_enable ? 0 : 1);

    /* available networks: scan button + result dropdown in one row */
    lv_obj_t *row = form_row("Available networks");
    dd_ssids = form_dd(row, "Tap Scan to search", 0);
    lv_obj_set_width(dd_ssids, 280);
    lv_obj_add_event_cb(dd_ssids, ssid_pick_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);
    btn_scan = lv_button_create(row);
    lv_obj_set_size(btn_scan, 120, 42);
    lv_obj_align(btn_scan, LV_ALIGN_RIGHT_MID, -292, 0);
    lv_obj_set_style_bg_color(btn_scan, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(btn_scan, COL_PANEL, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(btn_scan, 0, 0);
    lv_obj_add_event_cb(btn_scan, wscan_cb, LV_EVENT_CLICKED, NULL);
    lbl_scan = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan, LV_SYMBOL_REFRESH "  Scan");
    lv_obj_set_style_text_font(lbl_scan, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_scan, COL_BG, 0);
    lv_obj_set_style_text_color(lbl_scan, COL_MUTED, LV_STATE_DISABLED);
    lv_obj_center(lbl_scan);

    ta_ssid  = form_ta(form_row("Network name (SSID)"), g_cfg.wifi_ssid);
    ta_wpass = form_ta(form_row("Password"), g_cfg.wifi_pass);
    dd_wdhcp = form_dd(form_row("Address mode"),
                       "DHCP (automatic)\nStatic",
                       g_cfg.wifi_dhcp ? 0 : 1);
    ta_wip   = form_ta(form_row("IP address"), g_cfg.wifi_ip);
    ta_numeric(ta_wip);
    ta_wmask = form_ta(form_row("Subnet mask"), g_cfg.wifi_mask);
    ta_numeric(ta_wmask);
    ta_wgw   = form_ta(form_row("Gateway"), g_cfg.wifi_gw);
    ta_numeric(ta_wgw);

    page_save_button(LV_SYMBOL_SAVE "  Save", wifi_save_cb);

    lbl_wifires = lv_label_create(panel);
    lv_label_set_text(lbl_wifires, "");
    lv_obj_set_style_text_color(lbl_wifires, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_wifires, &font_units_14, 0);

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note,
        "Wired Ethernet and Wi-Fi can be used together; Modbus TCP is"
        " reachable on both. Wi-Fi applies on the recorder hardware.");
    lv_obj_set_style_text_color(note, COL_MUTED, 0);
    lv_obj_set_style_text_font(note, &font_units_12, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

/* ---- Self diagnostic page ---------------------------------------------- */

static lv_obj_t  *diag_list, *lbl_diagwhen, *diag_btn;
static lv_timer_t *diag_timer;

static void row_tx_anim(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, v, 0);
}

/* rows slide + fade in one after another */
static void diag_fill(int animate)
{
    lv_obj_clean(diag_list);
    lv_obj_set_flex_align(diag_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    diag_item_t it[DIAG_MAX];
    time_t when = 0;
    int n = diag_last(it, &when);

    if (n == 0) {
        lv_label_set_text(lbl_diagwhen, "Not run yet");
    } else {
        struct tm tm = *localtime(&when);
        lv_label_set_text_fmt(lbl_diagwhen, "Last run %02d:%02d:%02d",
                              tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(diag_list);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, it[i].name);
        lv_obj_set_style_text_font(nm, &font_units_14, 0);
        lv_obj_set_style_text_color(nm, COL_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *st = lv_label_create(row);
        lv_label_set_text(st, it[i].status == DIAG_OK   ? "PASS" :
                              it[i].status == DIAG_WARN ? "WARN" : "FAIL");
        lv_obj_set_style_text_font(st, &font_units_14, 0);
        lv_obj_set_style_text_color(st,
            it[i].status == DIAG_OK   ? COL_ACCENT :
            it[i].status == DIAG_WARN ? COL_MUTED  : COL_ALARM_TXT, 0);
        lv_obj_align(st, LV_ALIGN_LEFT_MID, 210, 0);

        lv_obj_t *dt = lv_label_create(row);
        lv_label_set_text(dt, it[i].detail);
        lv_obj_set_style_text_font(dt, &font_units_12, 0);
        lv_obj_set_style_text_color(dt, COL_MUTED, 0);
        lv_obj_set_width(dt, 440);
        lv_label_set_long_mode(dt, LV_LABEL_LONG_DOT);
        lv_obj_align(dt, LV_ALIGN_LEFT_MID, 285, 0);

        if (animate) {
            lv_obj_fade_in(row, 220, (uint32_t)i * 90);
            lv_obj_set_style_translate_x(row, 46, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, row);
            lv_anim_set_values(&a, 46, 0);
            lv_anim_set_duration(&a, 260);
            lv_anim_set_delay(&a, (uint32_t)i * 90);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, row_tx_anim);
            lv_anim_start(&a);
        }
    }
}

/* while the test runs: spinner + status in the results area */
static void diag_show_spinner(void)
{
    lv_obj_clean(diag_list);
    lv_obj_set_flex_align(diag_list, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sp = lv_spinner_create(diag_list);
    lv_obj_set_size(sp, 72, 72);
    lv_obj_set_style_arc_color(sp, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 7, LV_PART_INDICATOR);

    lv_obj_t *l = lv_label_create(diag_list);
    lv_label_set_text(l, "Running self test...");
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);

    lv_label_set_text(lbl_diagwhen, "Testing");
}

static void diag_poll(lv_timer_t *t)
{
    if (!diag_list || !lv_obj_is_valid(diag_list)) {
        lv_timer_delete(t);
        diag_timer = NULL;
        return;
    }
    if (diag_busy()) return;
    lv_timer_delete(t);
    diag_timer = NULL;
    lv_obj_remove_state(diag_btn, LV_STATE_DISABLED);
    diag_fill(1);
}

static void diag_run_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (diag_busy()) return;
    diag_show_spinner();
    lv_obj_add_state(diag_btn, LV_STATE_DISABLED);
    diag_run_async();          /* worker thread - UI keeps animating */
    diag_timer = lv_timer_create(diag_poll, 150, NULL);
}

static void build_diag_form(void)
{
    lv_obj_t *top = lv_obj_create(panel);
    lv_obj_set_size(top, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    diag_btn = lv_button_create(top);
    lv_obj_set_size(diag_btn, 190, 40);
    lv_obj_align(diag_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(diag_btn, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(diag_btn, COL_PANEL, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(diag_btn, 0, 0);
    lv_obj_add_event_cb(diag_btn, diag_run_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(diag_btn);
    lv_label_set_text(bl, LV_SYMBOL_CHARGE "  Run self test");
    lv_obj_set_style_text_font(bl, &font_units_14, 0);
    lv_obj_set_style_text_color(bl, COL_BG, 0);
    lv_obj_set_style_text_color(bl, COL_MUTED, LV_STATE_DISABLED);
    lv_obj_center(bl);

    lbl_diagwhen = lv_label_create(top);
    lv_obj_set_style_text_font(lbl_diagwhen, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_diagwhen, COL_MUTED, 0);
    lv_obj_align(lbl_diagwhen, LV_ALIGN_RIGHT_MID, 0, 0);

    diag_list = lv_obj_create(panel);
    lv_obj_set_size(diag_list, LV_PCT(100), 268);
    lv_obj_set_style_bg_color(diag_list, COL_PANEL, 0);
    lv_obj_set_style_border_color(diag_list, COL_BORDER, 0);
    lv_obj_set_style_border_width(diag_list, 1, 0);
    lv_obj_set_style_radius(diag_list, 8, 0);
    lv_obj_set_style_pad_all(diag_list, 10, 0);
    lv_obj_set_flex_flow(diag_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(diag_list, 2, 0);

    diag_fill(1);   /* animate the stored results on page open too */
}

/* ---- Account manager (user accounts; main menu while CFR is on) -------- */

static lv_obj_t *dd_accsel, *ta_accname, *ta_accpin,
                *dd_accrole, *dd_accact, *lbl_accinfo;
static int acc_sel;

static void acc_dd_refresh(void)
{
    char opts[8 * 24] = "";
    for (int i = 0; i < CFR_USERS; i++) {
        char row[24];
        snprintf(row, sizeof(row), "%s%d: %s", i ? "\n" : "", i + 1,
                 g_cfg.users[i].name[0] ? g_cfg.users[i].name : "(empty)");
        strcat(opts, row);
    }
    lv_dropdown_set_options(dd_accsel, opts);
    lv_dropdown_set_selected(dd_accsel, (uint32_t)acc_sel);
}

static void acc_load(void)
{
    form_loading = 1;
    cfr_user_t *u = &g_cfg.users[acc_sel];
    lv_textarea_set_text(ta_accname, u->name);
    lv_textarea_set_text(ta_accpin, u->pin);
    lv_dropdown_set_selected(dd_accrole,
                             (uint32_t)(u->role >= 0 && u->role <= 3
                                        ? u->role : 0));
    lv_dropdown_set_selected(dd_accact, u->active ? 0 : 1);

    /* slot 1 is the permanent administrator - can't be demoted or
     * disabled, so a lockout is impossible */
    if (acc_sel == 0) {
        lv_obj_add_state(dd_accrole, LV_STATE_DISABLED);
        lv_obj_add_state(dd_accact,  LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(dd_accrole, LV_STATE_DISABLED);
        lv_obj_remove_state(dd_accact,  LV_STATE_DISABLED);
    }
    form_loading = 0;
    save_set_dirty(0);
}

static void acc_sel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    acc_sel = (int)lv_dropdown_get_selected(dd_accsel);
    acc_load();
}

static void acc_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    cfr_user_t *u = &g_cfg.users[acc_sel];
    const char *newpin = lv_textarea_get_text(ta_accpin);
    /* §11.300: enforce the PIN policy on any non-empty PIN */
    if (newpin[0]) {
        char why[64];
        if (!cfr_pin_ok(newpin, why, sizeof(why))) {
            lv_label_set_text_fmt(lbl_accinfo, "Weak PIN: %s", why);
            lv_obj_set_style_text_color(lbl_accinfo, COL_ALARM_TXT, 0);
            return;
        }
    }
    int pin_changed = (newpin[0] && strcmp(newpin, u->pin) != 0);
    strncpy(u->name, lv_textarea_get_text(ta_accname), sizeof(u->name)-1);
    strncpy(u->pin,  newpin,  sizeof(u->pin)-1);
    if (pin_changed) u->pin_set = (int)(time(NULL) / 86400);  /* reset aging */
    u->role   = (int)lv_dropdown_get_selected(dd_accrole);
    u->active = lv_dropdown_get_selected(dd_accact) == 0 ? 1 : 0;
    if (acc_sel == 0) { u->role = ROLE_SUPERADMIN; u->active = 1; }
    if (!u->name[0] || !u->pin[0]) u->active = 0;

    config_save();
    event_log("CONFIG", "User account %d (%s) updated", acc_sel + 1,
              u->name[0] ? u->name : "empty");
    acc_dd_refresh();
    lv_label_set_text_fmt(lbl_accinfo, "Account %d saved.", acc_sel + 1);
    lv_obj_set_style_text_color(lbl_accinfo, COL_ACCENT, 0);
}

static void build_accounts_form(void)
{
    dd_accsel = form_dd(form_row("Account"), "1", 0);
    acc_dd_refresh();
    lv_obj_add_event_cb(dd_accsel, acc_sel_cb, LV_EVENT_VALUE_CHANGED,
                        NULL);

    ta_accname = form_ta(form_row("User name"), NULL);
    ta_accpin  = form_ta(form_row("PIN"), NULL);
    ta_numeric(ta_accpin);
    dd_accrole = form_dd(form_row("Access level"),
                         "Operator\nSupervisor\nAdmin\nSuper admin", 0);
    dd_accact  = form_dd(form_row("Status"), "Active\nDisabled", 1);

    page_save_button(LV_SYMBOL_SAVE "  Save", acc_save_cb);

    lbl_accinfo = lv_label_create(panel);
    lv_label_set_text_fmt(lbl_accinfo,
        "Logged in: %s. User 1 (SUPER ADMIN) is permanent."
        " Operator = view/ack/export; Supervisor = + channel, logging,"
        " network; Admin = + accounts; Super admin = + Factory"
        " settings.",
        cfr_logged_idx() >= 0 ? cfr_user_name() : "none");
    lv_obj_set_style_text_color(lbl_accinfo, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_accinfo, &font_units_12, 0);
    lv_obj_set_width(lbl_accinfo, LV_PCT(100));
    lv_label_set_long_mode(lbl_accinfo, LV_LABEL_LONG_WRAP);

    acc_load();
}

/* ---- 21 CFR mode switch (Factory settings) ----------------------------- */

static lv_obj_t *dd_cfren, *dd_esign, *dd_expiry, *lbl_cfrres;

static const int expiry_vals[] = { 0, 30, 60, 90 };

static void cfr_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int on = lv_dropdown_get_selected(dd_cfren) == 0 ? 1 : 0;
    if (on != g_cfg.cfr_enable)
        event_log("CONFIG", "21 CFR mode turned %s", on ? "ON" : "OFF");
    g_cfg.cfr_enable = on;
    g_cfg.esign_enable = lv_dropdown_get_selected(dd_esign) == 0 ? 1 : 0;
    g_cfg.pin_expiry_days = expiry_vals[lv_dropdown_get_selected(dd_expiry)];
    config_save();
    event_log("CONFIG", "21 CFR settings saved (e-sign %s; PIN expiry %d days)",
              g_cfg.esign_enable ? "on" : "off", g_cfg.pin_expiry_days);
    lv_label_set_text_fmt(lbl_cfrres,
        "21 CFR mode is %s.  E-signature %s.  PIN expiry %s.",
        on ? "ON" : "OFF", g_cfg.esign_enable ? "on" : "off",
        g_cfg.pin_expiry_days ? "on" : "off");
    lv_obj_set_style_text_color(lbl_cfrres, COL_ACCENT, 0);
}

static void build_cfr_form(void)
{
    dd_cfren = form_dd(form_row("21 CFR mode"), "Enabled\nDisabled",
                       g_cfg.cfr_enable ? 0 : 1);
    dd_esign = form_dd(form_row("Electronic signature on report"),
                       "Enabled\nDisabled", g_cfg.esign_enable ? 0 : 1);
    int esel = 0;
    for (int i = 0; i < 4; i++)
        if (expiry_vals[i] == g_cfg.pin_expiry_days) esel = i;
    dd_expiry = form_dd(form_row("PIN expiry"),
                        "Off\n30 days\n60 days\n90 days", esel);

    page_save_button(LV_SYMBOL_SAVE "  Save", cfr_save_cb);

    lbl_cfrres = lv_label_create(panel);
    lv_label_set_text(lbl_cfrres, "");
    lv_obj_set_style_text_color(lbl_cfrres, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_cfrres, &font_units_14, 0);
    lv_obj_set_width(lbl_cfrres, LV_PCT(100));
    lv_label_set_long_mode(lbl_cfrres, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note,
        "When enabled: signing in is required to open the Menu, every"
        " action in the event log carries the user's name, and user"
        " accounts are managed from the main-menu Account manager"
        " (built-in account: SUPER ADMIN / 1234).");
    lv_obj_set_style_text_color(note, COL_MUTED, 0);
    lv_obj_set_style_text_font(note, &font_units_12, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

static void log_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    g_cfg.store_interval = interval_vals[lv_dropdown_get_selected(dd_interval)];
    config_save();
    event_log("CONFIG", "Store interval set to %d s", g_cfg.store_interval);
}

static void build_logging_form(void)
{
    int sel = 0;
    for (int i = 0; i < 6; i++)
        if (interval_vals[i] == g_cfg.store_interval) sel = i;

    dd_interval = form_dd(form_row("Store interval"),
        "1 minute\n5 minutes\n10 minutes\n15 minutes\n"
        "30 minutes\n1 hour", sel);
    page_save_button(LV_SYMBOL_SAVE "  Save", log_save_cb);
}

/* ---- data export -------------------------------------------------------- */

/* ---- data export: calendar-based range selection ---- */

static time_t exp_t0, exp_t1;
static lv_obj_t *btn_t0, *btn_t1;

static void exp_fields_update(void)
{
    struct tm a = *localtime(&exp_t0);
    struct tm b = *localtime(&exp_t1);
    lv_label_set_text_fmt(lv_obj_get_child(btn_t0, 0),
        "%04d-%02d-%02d   %02d:%02d",
        a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
    lv_label_set_text_fmt(lv_obj_get_child(btn_t1, 0),
        "%04d-%02d-%02d   %02d:%02d",
        b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);
}

static void usb_status_update(void)
{
    char usb[128];
    if (usb_find(usb, sizeof(usb))) {
        lv_label_set_text_fmt(lbl_usb, LV_SYMBOL_USB "  USB drive ready (%s)", usb);
        lv_obj_set_style_text_color(lbl_usb, COL_ACCENT, 0);
    } else {
        lv_label_set_text(lbl_usb, LV_SYMBOL_USB "  No USB drive connected");
        lv_obj_set_style_text_color(lbl_usb, COL_MUTED, 0);
    }
}

static void quick_today_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    exp_t0 = mktime(&tm);
    exp_t1 = now;
    exp_fields_update();
}

static void quick_hour_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    time_t now = time(NULL);
    exp_t0 = now - 3600;
    exp_t1 = now;
    exp_fields_update();
}

static void quick_day_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    time_t now = time(NULL);
    exp_t0 = now - 86400;
    exp_t1 = now;
    exp_fields_update();
}

/* ---- calendar + time picker dialog ---- */

static lv_obj_t *pk_ov, *pk_cal, *pk_h, *pk_m;
static lv_calendar_date_t pk_date;
static int pk_target;   /* 0 = start, 1 = end */

static void pk_close(void)
{
    if (pk_ov) { lv_obj_delete(pk_ov); pk_ov = NULL; }
}

static void pk_cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    pk_close();
}

static void pk_cal_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    lv_calendar_date_t d;
    if (lv_calendar_get_pressed_date(pk_cal, &d) == LV_RESULT_OK) {
        pk_date = d;
        lv_calendar_set_highlighted_dates(pk_cal, &pk_date, 1);
    }
}

static void pk_ok_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    struct tm tm = {0};
    tm.tm_year = pk_date.year - 1900;
    tm.tm_mon  = pk_date.month - 1;
    tm.tm_mday = pk_date.day;
    tm.tm_hour = (int)lv_roller_get_selected(pk_h);
    tm.tm_min  = (int)lv_roller_get_selected(pk_m);
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);

    if (pk_target == 0) exp_t0 = t;
    else                exp_t1 = t;
    exp_fields_update();
    pk_close();
}

static void pk_open(int target)
{
    static const char *hours =
        "00\n01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n"
        "12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23";
    static const char *mins =
        "00\n05\n10\n15\n20\n25\n30\n35\n40\n45\n50\n55";

    pk_target = target;
    time_t cur = target == 0 ? exp_t0 : exp_t1;
    struct tm tm = *localtime(&cur);

    pk_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(pk_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(pk_ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pk_ov, LV_OPA_50, 0);
    lv_obj_set_style_border_width(pk_ov, 0, 0);
    lv_obj_set_style_radius(pk_ov, 0, 0);
    lv_obj_remove_flag(pk_ov, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dlg = lv_obj_create(pk_ov);
    lv_obj_set_size(dlg, 520, 410);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, COL_PANEL, 0);
    lv_obj_set_style_border_color(dlg, COL_BORDER, 0);
    lv_obj_set_style_radius(dlg, 10, 0);
    lv_obj_set_style_pad_all(dlg, 10, 0);
    lv_obj_remove_flag(dlg, LV_OBJ_FLAG_SCROLLABLE);

    /* calendar with month arrows */
    pk_cal = lv_calendar_create(dlg);
    lv_obj_set_size(pk_cal, 340, 340);
    lv_obj_set_pos(pk_cal, 0, 0);
    /* tap the year / month to select directly (mobile style) */
    lv_calendar_header_dropdown_create(pk_cal);
    lv_calendar_header_dropdown_set_year_list(pk_cal,
        "2027\n2026\n2025\n2024");
    lv_calendar_set_today_date(pk_cal, (uint32_t)(tm.tm_year + 1900),
                               (uint32_t)(tm.tm_mon + 1),
                               (uint32_t)tm.tm_mday);
    lv_calendar_set_showed_date(pk_cal, (uint32_t)(tm.tm_year + 1900),
                                (uint32_t)(tm.tm_mon + 1));
    pk_date.year  = (uint16_t)(tm.tm_year + 1900);
    pk_date.month = (int8_t)(tm.tm_mon + 1);
    pk_date.day   = (int8_t)tm.tm_mday;
    lv_calendar_set_highlighted_dates(pk_cal, &pk_date, 1);
    lv_obj_add_event_cb(pk_cal, pk_cal_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* time rollers */
    lv_obj_t *lh = lv_label_create(dlg);
    lv_label_set_text(lh, "Hour");
    lv_obj_set_style_text_font(lh, &font_units_12, 0);
    lv_obj_set_style_text_color(lh, COL_MUTED, 0);
    lv_obj_set_pos(lh, 366, 0);

    pk_h = lv_roller_create(dlg);
    lv_roller_set_options(pk_h, hours, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(pk_h, 4);
    lv_obj_set_width(pk_h, 62);
    lv_obj_set_pos(pk_h, 362, 22);
    lv_roller_set_selected(pk_h, (uint32_t)tm.tm_hour, LV_ANIM_OFF);

    lv_obj_t *lm = lv_label_create(dlg);
    lv_label_set_text(lm, "Min");
    lv_obj_set_style_text_font(lm, &font_units_12, 0);
    lv_obj_set_style_text_color(lm, COL_MUTED, 0);
    lv_obj_set_pos(lm, 438, 0);

    pk_m = lv_roller_create(dlg);
    lv_roller_set_options(pk_m, mins, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(pk_m, 4);
    lv_obj_set_width(pk_m, 62);
    lv_obj_set_pos(pk_m, 434, 22);
    lv_roller_set_selected(pk_m, (uint32_t)(tm.tm_min / 5), LV_ANIM_OFF);

    /* OK / Cancel */
    lv_obj_t *ok = lv_button_create(dlg);
    lv_obj_set_size(ok, 134, 48);
    lv_obj_set_pos(ok, 362, 270);
    lv_obj_set_style_bg_color(ok, COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(ok, 0, 0);
    lv_obj_add_event_cb(ok, pk_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *okl = lv_label_create(ok);
    lv_label_set_text(okl, LV_SYMBOL_OK "  OK");
    lv_obj_set_style_text_color(okl, COL_BG, 0);
    lv_obj_center(okl);

    lv_obj_t *cc = lv_button_create(dlg);
    lv_obj_set_size(cc, 134, 48);
    lv_obj_set_pos(cc, 362, 330);
    lv_obj_set_style_bg_color(cc, COL_PANEL, 0);
    lv_obj_set_style_border_color(cc, COL_BORDER, 0);
    lv_obj_set_style_border_width(cc, 1, 0);
    lv_obj_set_style_shadow_width(cc, 0, 0);
    lv_obj_add_event_cb(cc, pk_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ccl = lv_label_create(cc);
    lv_label_set_text(ccl, "Cancel");
    lv_obj_set_style_text_color(ccl, COL_TEXT, 0);
    lv_obj_center(ccl);
}

static void pick_start_cb(lv_event_t *e) { LV_UNUSED(e); pk_open(0); }
static void pick_end_cb(lv_event_t *e)   { LV_UNUSED(e); pk_open(1); }

static void export_common(int alarms)
{
    char msg[128];

    int rc = alarms == 1
           ? export_alarms_range(exp_t0, exp_t1 + 59, msg, sizeof(msg))
           : alarms == 2
           ? export_events_range(exp_t0, exp_t1 + 59, msg, sizeof(msg))
           : export_range(exp_t0, exp_t1 + 59, msg, sizeof(msg));
    if (rc > 0)
        event_log("EXPORT", "%s", msg);
    lv_label_set_text(lbl_result, msg);
    lv_obj_set_style_text_color(lbl_result,
                                rc > 0 ? COL_ACCENT : COL_ALARM_TXT, 0);
    usb_status_update();
}

static void export_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    export_common(0);
}

static void export_alarms_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    export_common(1);
}

static void export_events_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    export_common(2);
}

static void export_report_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char msg[160];
    lv_label_set_text(lbl_result, "Generating PDF report...");
    lv_obj_set_style_text_color(lbl_result, COL_MUTED, 0);
    lv_refr_now(NULL);   /* show the progress note before the (blocking) build */
    int rc = export_report_pdf(exp_t0, exp_t1 + 59, msg, sizeof(msg));
    if (rc > 0) event_log("EXPORT", "PDF report - %s", msg);
    lv_label_set_text(lbl_result, msg);
    lv_obj_set_style_text_color(lbl_result,
                                rc > 0 ? COL_ACCENT : COL_ALARM_TXT, 0);
    usb_status_update();
}

static void export_verify_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    char msg[128], date[11];
    struct tm b = *localtime(&exp_t1);
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
    int rc = event_audit_verify(date, msg, sizeof(msg));
    lv_label_set_text(lbl_result, msg);
    lv_obj_set_style_text_color(lbl_result,
                                rc == 0 ? COL_ACCENT : COL_ALARM_TXT, 0);
}

/* absolute-position helpers for the single-screen export page */
static lv_obj_t *exp_btn(int x, int y, int w, const char *txt,
                         lv_event_cb_t cb, bool primary)
{
    lv_obj_t *b = lv_button_create(panel);
    lv_obj_set_size(b, w, 46);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, primary ? COL_ACCENT : COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, primary ? 0 : 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, primary ? COL_BG : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *exp_lbl(int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(panel);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    return l;
}

static void build_export_form(void)
{
    /* single fixed screen: range selection left, export actions right */
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 12, 0);

    /* ---- left: time range ---- */
    lbl_usb = lv_label_create(panel);
    lv_obj_set_style_text_font(lbl_usb, &font_units_14, 0);
    lv_obj_set_pos(lbl_usb, 0, 0);
    usb_status_update();

    exp_lbl(0, 54, "Start");
    btn_t0 = exp_btn(62, 40, 250, "-", pick_start_cb, false);

    exp_lbl(0, 110, "End");
    btn_t1 = exp_btn(62, 96, 250, "-", pick_end_cb, false);

    if (exp_t0 == 0 || exp_t1 == 0) quick_today_cb(NULL);
    else exp_fields_update();

    exp_btn(0, 158, 98, "Today", quick_today_cb, false);
    exp_btn(106, 158, 98, "1 h", quick_hour_cb, false);
    exp_btn(212, 158, 100, "24 h", quick_day_cb, false);

    lbl_result = lv_label_create(panel);
    lv_label_set_text(lbl_result, "");
    lv_obj_set_style_text_font(lbl_result, &font_units_14, 0);
    lv_obj_set_pos(lbl_result, 0, 224);
    lv_obj_set_width(lbl_result, 360);
    lv_label_set_long_mode(lbl_result, LV_LABEL_LONG_WRAP);

    /* ---- right: what to export ---- */
    exp_lbl(410, 0, "Write to USB stick");
    exp_btn(410, 24, 330, LV_SYMBOL_SAVE "  PDF report", export_report_cb, true);
    exp_btn(410, 78, 330, LV_SYMBOL_USB "  PV data (CSV)", export_cb, false);
    exp_btn(410, 132, 330, LV_SYMBOL_BELL "  Alarm history (CSV)",
            export_alarms_cb, false);
    exp_btn(410, 186, 330, LV_SYMBOL_FILE "  Event log (CSV)",
            export_events_cb, false);

    exp_btn(410, 240, 330, LV_SYMBOL_OK "  Verify audit trail (End day)",
            export_verify_cb, false);

    lv_obj_t *note = exp_lbl(410, 292,
        "PDF report matches the web dashboard. Verify checks the\n"
        "tamper-evident hash chain of the day's audit trail.");
    lv_obj_set_style_text_font(note, &font_units_12, 0);
}

/* ---- card configuration (per-channel input types, live values) ---------- */

static void cfg_val_text(int16_t raw, char *out, int n)
{
    if      (raw == 32764) snprintf(out, n, "SKIP");
    else if (raw == 32765) snprintf(out, n, "UNDER");
    else if (raw == 32766) snprintf(out, n, "OVER");
    else if (raw == 32767) snprintf(out, n, "OPEN");
    else                   snprintf(out, n, "%.1f", (double)raw / 10.0);
}

static void cardcfg_read_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int card  = (int)lv_dropdown_get_selected(dd_cfg_card);
    int slave = g_cfg.slave_base + card;

    uint16_t types[8], vals[9];
    int t_ok = (mb_service_read(slave, 3, REG_TYPE, 8, types) == 0);
    int v_ok = (mb_service_read(slave, 4, REG_PV, 9, vals) == 0);

    if (!t_ok && !v_ok) {
        lv_label_set_text_fmt(lbl_cfgres, "Card %d (slave %d): no response",
                              card + 1, slave);
        lv_obj_set_style_text_color(lbl_cfgres, COL_ALARM_TXT, 0);
        return;
    }
    for (int c = 0; c < 8; c++) {
        if (t_ok && types[c] < 20)
            lv_dropdown_set_selected(dd_cfgtype[c], types[c]);
        if (v_ok) {
            char v[16];
            cfg_val_text((int16_t)vals[c], v, sizeof(v));
            lv_label_set_text(lbl_cfgval[c], v);
        }
    }
    if (v_ok)
        lv_label_set_text_fmt(lbl_cfgres, "Card %d read OK   CJC %.1f",
                              card + 1, (double)(int16_t)vals[8] / 10.0);
    else
        lv_label_set_text_fmt(lbl_cfgres, "Card %d types read", card + 1);
    lv_obj_set_style_text_color(lbl_cfgres, COL_ACCENT, 0);
}

static void cardcfg_write_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int card  = (int)lv_dropdown_get_selected(dd_cfg_card);
    int slave = g_cfg.slave_base + card;

    int fails = 0;
    for (int c = 0; c < 8; c++) {
        uint16_t ty = (uint16_t)lv_dropdown_get_selected(dd_cfgtype[c]);
        if (mb_service_write(slave, REG_TYPE + c, ty) != 0) fails++;
    }
    comm_refresh_types(card);
    event_log("CONFIG", "Card %d input types written (%d ok)",
              card + 1, 8 - fails);
    if (fails == 0) {
        lv_label_set_text(lbl_cfgres, "All 8 input types written (verified)");
        lv_obj_set_style_text_color(lbl_cfgres, COL_ACCENT, 0);
    } else {
        lv_label_set_text_fmt(lbl_cfgres, "%d of 8 writes failed", fails);
        lv_obj_set_style_text_color(lbl_cfgres, COL_ALARM_TXT, 0);
    }
}

static void build_cardcfg_form(void)
{
    char cards_opt[16];
    int len = 0;
    cards_opt[0] = 0;
    for (int i = 0; i < g_cfg.cards && len < (int)sizeof(cards_opt) - 3; i++)
        len += snprintf(cards_opt + len, sizeof(cards_opt) - (size_t)len,
                        "%d%s", i + 1, i < g_cfg.cards - 1 ? "\n" : "");

    char topts[300];
    int tlen = 0;
    topts[0] = 0;
    for (unsigned t = 0; t < 20 && tlen < (int)sizeof(topts) - 1; t++)
        tlen += snprintf(topts + tlen, sizeof(topts) - (size_t)tlen, "%s%s",
                         mb_type_name(t), t < 19 ? "\n" : "");

    dd_cfg_card = form_dd(form_row("Card"), cards_opt, 0);

    for (int c = 0; c < 8; c++) {
        char rn[8];
        snprintf(rn, sizeof(rn), "CH%d", c + 1);
        lv_obj_t *row = form_row(rn);

        dd_cfgtype[c] = lv_dropdown_create(row);
        lv_dropdown_set_options(dd_cfgtype[c], topts);
        lv_obj_set_size(dd_cfgtype[c], 180, 40);
        lv_obj_align(dd_cfgtype[c], LV_ALIGN_RIGHT_MID, -160, 0);
        lv_obj_set_style_bg_color(dd_cfgtype[c], COL_BG, 0);
        lv_obj_set_style_text_color(dd_cfgtype[c], COL_TEXT, 0);
        lv_obj_set_style_border_color(dd_cfgtype[c], COL_BORDER, 0);
        lv_obj_t *dl = lv_dropdown_get_list(dd_cfgtype[c]);
        lv_obj_set_style_bg_color(dl, COL_PANEL, 0);
        lv_obj_set_style_text_color(dl, COL_TEXT, 0);
        lv_obj_set_style_max_height(dl, 200, 0);

        lbl_cfgval[c] = lv_label_create(row);
        lv_label_set_text(lbl_cfgval[c], "-");
        lv_obj_set_style_text_font(lbl_cfgval[c], &font_units_14, 0);
        lv_obj_set_style_text_color(lbl_cfgval[c], COL_TEXT, 0);
        lv_obj_set_style_text_align(lbl_cfgval[c], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(lbl_cfgval[c], 140);
        lv_obj_align(lbl_cfgval[c], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    action_button(LV_SYMBOL_REFRESH "  Read card", cardcfg_read_cb);
    action_button(LV_SYMBOL_UPLOAD "  Write input types", cardcfg_write_cb);

    lbl_cfgres = lv_label_create(panel);
    lv_label_set_text(lbl_cfgres, "Types and live values per channel");
    lv_obj_set_style_text_font(lbl_cfgres, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_cfgres, COL_MUTED, 0);

    if (g_cfg.source == SRC_MODBUS) cardcfg_read_cb(NULL);
}

static void reg_read_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    int slave = g_cfg.slave_base + card;
    int fc    = lv_dropdown_get_selected(dd_rfc) == 0 ? 3 : 4;
    int addr  = atoi(lv_textarea_get_text(ta_raddr));
    static const int cnts[] = { 1, 2, 4, 8 };
    int n = cnts[lv_dropdown_get_selected(dd_rcnt)];

    uint16_t regs[8];
    if (mb_service_read(slave, fc, addr, n, regs) != 0) {
        lv_label_set_text(lbl_rres, "Read failed");
        lv_obj_set_style_text_color(lbl_rres, COL_ALARM_TXT, 0);
        return;
    }
    char buf[256];
    int len = 0;
    buf[0] = 0;
    /* clamp: snprintf returns the INTENDED length, so len can run past
     * the buffer; stop before buf+len / sizeof(buf)-len go out of range */
    for (int i = 0; i < n && len < (int)sizeof(buf) - 1; i++)
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "reg %d = %u  (0x%04X, i16 %d)\n",
                        addr + i, regs[i], regs[i], (int16_t)regs[i]);
    lv_label_set_text(lbl_rres, buf);
    lv_obj_set_style_text_color(lbl_rres, COL_ACCENT, 0);
}

static void reg_write_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!lv_obj_has_state(sw_arm, LV_STATE_CHECKED)) {
        lv_label_set_text(lbl_wres, "Enable the write switch first");
        lv_obj_set_style_text_color(lbl_wres, COL_ALARM_TXT, 0);
        return;
    }
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    int slave = g_cfg.slave_base + card;
    int addr  = atoi(lv_textarea_get_text(ta_waddr));
    int val   = atoi(lv_textarea_get_text(ta_wval));

    if (mb_service_write(slave, addr, (uint16_t)val) == 0) {
        event_log("CONFIG", "Register write; slave %d; reg %d = %d",
                  slave, addr, val);
        lv_label_set_text_fmt(lbl_wres, "Written: reg %d = %d (verified)",
                              addr, val);
        lv_obj_set_style_text_color(lbl_wres, COL_ACCENT, 0);
    } else {
        lv_label_set_text(lbl_wres, "Write failed");
        lv_obj_set_style_text_color(lbl_wres, COL_ALARM_TXT, 0);
    }
    lv_obj_remove_state(sw_arm, LV_STATE_CHECKED);   /* one write per arm */
}

/* ---- guided calibration (card regs 160..163, one channel per card;
 * the card shares the result to all channels of the same family) ---- */

static void cal_status(const char *txt, int good)
{
    lv_label_set_text(lbl_calstat, txt);
    lv_obj_set_style_text_color(lbl_calstat,
        good > 0 ? COL_ACCENT : good < 0 ? COL_ALARM_TXT : COL_TEXT, 0);
}

static void cal_buttons_enable(int en)
{
    if (en) {
        lv_obj_remove_state(btn_cap_lo, LV_STATE_DISABLED);
        lv_obj_remove_state(btn_cap_hi, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn_cap_lo, LV_STATE_DISABLED);
        lv_obj_add_state(btn_cap_hi, LV_STATE_DISABLED);
    }
}

static void cal_start_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    int slave = g_cfg.slave_base + card;
    int type  = (int)lv_dropdown_get_selected(dd_caltype) + 1; /* skip 0 */

    /* CH1 is the calibration channel; result is shared automatically */
    if (mb_service_write(slave, REG_CAL_SELECT,
                         (uint16_t)(1000 + type)) == 0) {
        cal_active = 1;
        cal_slave  = slave;
        cal_buttons_enable(1);
        event_log("CAL", "Calibration started; card %d; type %s",
                  (int)lv_dropdown_get_selected(dd_scard) + 1,
                  mb_type_name((unsigned)type));
        cal_status("Step 1: connect the LOW reference to CH1,"
                   " wait until the count is stable,"
                   " then press Capture LOW", 0);
    } else {
        cal_status("Card not responding - calibration not started", -1);
    }
}

static void cal_low_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!cal_active) { cal_status("Press Start calibration first", -1); return; }
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    int slave = g_cfg.slave_base + card;
    float ref = (float)atof(lv_textarea_get_text(ta_clo));

    if (mb_service_write(slave, REG_CAL_LO,
                         (uint16_t)(int16_t)(ref * 100.0f)) == 0) {
        cal_status("LOW captured. Step 2: connect the HIGH reference,"
                   " wait until stable, then press Capture HIGH", 1);
        event_log("CAL", "LOW point captured; ref %g", (double)ref);
    } else {
        cal_status("LOW capture failed", -1);
    }
}

static void cal_high_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (!cal_active) { cal_status("Press Start calibration first", -1); return; }
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    int slave = g_cfg.slave_base + card;
    float ref = (float)atof(lv_textarea_get_text(ta_chi));

    if (mb_service_write(slave, REG_CAL_HI,
                         (uint16_t)(int16_t)(ref * 100.0f)) != 0) {
        cal_status("HIGH capture failed", -1);
        return;
    }
    /* card commits when both points exist - check the valid flag */
    uint16_t valid = 0;
    mb_service_read(slave, 3, REG_CAL_VALID, 1, &valid);
    if (valid) {
        mb_service_write(slave, REG_CAL_SELECT, 0);   /* end session */
        cal_active = 0;
        cal_buttons_enable(0);
        cal_status("Calibration COMPLETE and stored on the card"
                   " (shared to all channels of this input family)", 1);
        event_log("CAL", "HIGH point captured; ref %g; calibration COMPLETE",
                  (double)ref);
    } else {
        cal_status("HIGH captured. If LOW is also done the card"
                   " commits automatically - check valid flag", 0);
    }
}

static void cal_end_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int card  = (int)lv_dropdown_get_selected(dd_scard);
    mb_service_write(g_cfg.slave_base + card, REG_CAL_SELECT, 0);
    cal_active = 0;
    cal_buttons_enable(0);
    cal_status("Calibration stopped - card unchanged", 0);
    event_log("CAL", "Calibration stopped by operator");
    lv_label_set_text(lbl_calcount, "-");
}

/* typical calibration reference points per input type */
static void cal_type_change_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    int type = (int)lv_dropdown_get_selected(dd_caltype) + 1;
    const char *lo = "0", *hi = "100";

    switch (type) {
    case 12: lo = "0"; hi = "20";  break;   /* 0-20 mA */
    case 13: lo = "4"; hi = "20";  break;   /* 4-20 mA */
    case 14: lo = "0"; hi = "300"; break;   /* ohms    */
    case 15: lo = "0"; hi = "50";  break;   /* 50 mV   */
    case 16: lo = "0"; hi = "100"; break;   /* 100 mV  */
    case 17: lo = "0"; hi = "250"; break;   /* 250 mV  */
    case 18: lo = "0"; hi = "5";   break;   /* 0-5 V   */
    case 19: lo = "0"; hi = "10";  break;   /* 0-10 V  */
    default: break;                          /* TC/RTD: 0 and 100 */
    }
    lv_textarea_set_text(ta_clo, lo);
    lv_textarea_set_text(ta_chi, hi);
}

/* absolutely-positioned button for the single-screen calibration page */
static lv_obj_t *cal_btn(int x, int y, int w, const char *txt,
                         lv_event_cb_t cb, bool primary)
{
    lv_obj_t *btn = lv_button_create(panel);
    lv_obj_set_size(btn, w, 44);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, primary ? COL_ACCENT : COL_PANEL, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, primary ? 0 : 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, primary ? COL_BG : COL_TEXT, 0);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *cal_lbl(int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(panel);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    return l;
}

/* leaving the calibration page abandons any running session: clear the
 * card's cal-select register so it doesn't stay stuck in calibration
 * mode. Uses the captured slave, not the (being-deleted) dropdown. */
static void cal_page_delete_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (cal_active) {
        mb_service_write(cal_slave, REG_CAL_SELECT, 0);
        cal_active = 0;
        event_log("CAL", "Calibration page left - session ended");
    }
}

static void build_cal_form(void)
{
    /* single fixed screen: no flex, no scrolling */
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, cal_page_delete_cb, LV_EVENT_DELETE, NULL);

    char cards_opt[16];
    int len = 0;
    cards_opt[0] = 0;
    for (int i = 0; i < g_cfg.cards && len < (int)sizeof(cards_opt) - 3; i++)
        len += snprintf(cards_opt + len, sizeof(cards_opt) - (size_t)len,
                        "%d%s", i + 1, i < g_cfg.cards - 1 ? "\n" : "");

    char topts[280];
    int tlen = 0;
    topts[0] = 0;
    for (unsigned t = 1; t < 20 && tlen < (int)sizeof(topts) - 1; t++)
        tlen += snprintf(topts + tlen, sizeof(topts) - (size_t)tlen, "%s%s",
                         mb_type_name(t), t < 19 ? "\n" : "");

    /* ---- left column: controls ---- */
    cal_lbl(0, 12, "Card");
    dd_scard = lv_dropdown_create(panel);
    lv_dropdown_set_options(dd_scard, cards_opt);
    lv_obj_set_size(dd_scard, 74, 42);
    lv_obj_set_pos(dd_scard, 48, 0);
    lv_obj_set_style_bg_color(dd_scard, COL_BG, 0);
    lv_obj_set_style_text_color(dd_scard, COL_TEXT, 0);
    lv_obj_set_style_border_color(dd_scard, COL_BORDER, 0);
    lv_obj_t *sl = lv_dropdown_get_list(dd_scard);
    lv_obj_set_style_bg_color(sl, COL_PANEL, 0);
    lv_obj_set_style_text_color(sl, COL_TEXT, 0);

    cal_lbl(140, 12, "Type");
    dd_caltype = lv_dropdown_create(panel);
    lv_dropdown_set_options(dd_caltype, topts);
    lv_dropdown_set_selected(dd_caltype, 8);   /* Pt-100 */
    lv_obj_set_size(dd_caltype, 180, 42);
    lv_obj_set_pos(dd_caltype, 190, 0);
    lv_obj_set_style_bg_color(dd_caltype, COL_BG, 0);
    lv_obj_set_style_text_color(dd_caltype, COL_TEXT, 0);
    lv_obj_set_style_border_color(dd_caltype, COL_BORDER, 0);
    lv_obj_t *tl = lv_dropdown_get_list(dd_caltype);
    lv_obj_set_style_bg_color(tl, COL_PANEL, 0);
    lv_obj_set_style_text_color(tl, COL_TEXT, 0);
    lv_obj_set_style_max_height(tl, 200, 0);
    lv_obj_add_event_cb(dd_caltype, cal_type_change_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    cal_btn(0, 58, 180, LV_SYMBOL_PLAY "  Start", cal_start_cb, true);
    cal_btn(190, 58, 180, LV_SYMBOL_STOP "  Stop calibration",
            cal_end_cb, false);

    /* step 1: LOW */
    lv_obj_t *bd1 = lv_obj_create(panel);
    lv_obj_set_size(bd1, 26, 26);
    lv_obj_set_pos(bd1, 0, 128);
    lv_obj_set_style_radius(bd1, 13, 0);
    lv_obj_set_style_bg_color(bd1, COL_ACCENT, 0);
    lv_obj_set_style_border_width(bd1, 0, 0);
    lv_obj_remove_flag(bd1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bn1 = lv_label_create(bd1);
    lv_label_set_text(bn1, "1");
    lv_obj_set_style_text_color(bn1, COL_BG, 0);
    lv_obj_center(bn1);

    cal_lbl(36, 132, "LOW ref");
    ta_clo = lv_textarea_create(panel);
    lv_textarea_set_one_line(ta_clo, true);
    lv_textarea_set_text(ta_clo, "0");
    lv_obj_set_size(ta_clo, 90, 42);
    lv_obj_set_pos(ta_clo, 100, 120);
    lv_obj_set_style_bg_color(ta_clo, COL_BG, 0);
    lv_obj_set_style_text_color(ta_clo, COL_TEXT, 0);
    lv_obj_set_style_border_color(ta_clo, COL_BORDER, 0);
    lv_obj_add_event_cb(ta_clo, ta_event_cb, LV_EVENT_ALL, NULL);
    ta_numeric(ta_clo);
    btn_cap_lo = cal_btn(200, 120, 170, LV_SYMBOL_DOWNLOAD "  Capture LOW",
                         cal_low_cb, false);
    lv_obj_set_style_opa(btn_cap_lo, LV_OPA_40, LV_STATE_DISABLED);

    /* step 2: HIGH */
    lv_obj_t *bd2 = lv_obj_create(panel);
    lv_obj_set_size(bd2, 26, 26);
    lv_obj_set_pos(bd2, 0, 190);
    lv_obj_set_style_radius(bd2, 13, 0);
    lv_obj_set_style_bg_color(bd2, COL_ACCENT, 0);
    lv_obj_set_style_border_width(bd2, 0, 0);
    lv_obj_remove_flag(bd2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bn2 = lv_label_create(bd2);
    lv_label_set_text(bn2, "2");
    lv_obj_set_style_text_color(bn2, COL_BG, 0);
    lv_obj_center(bn2);

    cal_lbl(36, 194, "HIGH ref");
    ta_chi = lv_textarea_create(panel);
    lv_textarea_set_one_line(ta_chi, true);
    lv_textarea_set_text(ta_chi, "100");
    lv_obj_set_size(ta_chi, 90, 42);
    lv_obj_set_pos(ta_chi, 100, 182);
    lv_obj_set_style_bg_color(ta_chi, COL_BG, 0);
    lv_obj_set_style_text_color(ta_chi, COL_TEXT, 0);
    lv_obj_set_style_border_color(ta_chi, COL_BORDER, 0);
    lv_obj_add_event_cb(ta_chi, ta_event_cb, LV_EVENT_ALL, NULL);
    ta_numeric(ta_chi);
    btn_cap_hi = cal_btn(200, 182, 170, LV_SYMBOL_DOWNLOAD "  Capture HIGH",
                         cal_high_cb, false);
    lv_obj_set_style_opa(btn_cap_hi, LV_OPA_40, LV_STATE_DISABLED);

    /* ---- right column: live count + status ---- */
    lv_obj_t *cbox = lv_obj_create(panel);
    lv_obj_set_size(cbox, 330, 96);
    lv_obj_set_pos(cbox, 400, 0);
    lv_obj_set_style_bg_color(cbox, COL_PANEL, 0);
    lv_obj_set_style_border_color(cbox, COL_BORDER, 0);
    lv_obj_set_style_radius(cbox, 8, 0);
    lv_obj_remove_flag(cbox, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cc = lv_label_create(cbox);
    lv_label_set_text(cc, "ADC count (CH1)");
    lv_obj_set_style_text_font(cc, &font_units_12, 0);
    lv_obj_set_style_text_color(cc, COL_MUTED, 0);
    lv_obj_align(cc, LV_ALIGN_TOP_LEFT, 6, 2);

    lbl_calcount = lv_label_create(cbox);
    lv_label_set_text(lbl_calcount, "-");
    lv_obj_set_style_text_font(lbl_calcount, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl_calcount, COL_ACCENT, 0);
    lv_obj_align(lbl_calcount, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t *sbox = lv_obj_create(panel);
    lv_obj_set_size(sbox, 330, 130);
    lv_obj_set_pos(sbox, 400, 108);
    lv_obj_set_style_bg_color(sbox, COL_PANEL, 0);
    lv_obj_set_style_border_color(sbox, COL_BORDER, 0);
    lv_obj_set_style_radius(sbox, 8, 0);
    lv_obj_set_style_pad_all(sbox, 10, 0);
    lv_obj_remove_flag(sbox, LV_OBJ_FLAG_SCROLLABLE);

    lbl_calstat = lv_label_create(sbox);
    lv_label_set_text(lbl_calstat,
        "Select type, press Start. Apply references to CH1 and capture"
        " LOW then HIGH. Result is shared to all channels of the"
        " same input family.");
    lv_obj_set_style_text_font(lbl_calstat, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_calstat, COL_MUTED, 0);
    lv_obj_set_width(lbl_calstat, LV_PCT(100));
    lv_label_set_long_mode(lbl_calstat, LV_LABEL_LONG_WRAP);

    /* captures stay disabled until a session is started */
    cal_buttons_enable(cal_active ? 1 : 0);
}

static void build_regs_form(void)
{
    char cards_opt[16];
    int len = 0;
    cards_opt[0] = 0;
    for (int i = 0; i < g_cfg.cards && len < (int)sizeof(cards_opt) - 3; i++)
        len += snprintf(cards_opt + len, sizeof(cards_opt) - (size_t)len,
                        "%d%s", i + 1, i < g_cfg.cards - 1 ? "\n" : "");

    dd_scard = form_dd(form_row("Card"), cards_opt, 0);

    lv_obj_t *div1 = lv_label_create(panel);
    lv_label_set_text(div1, "Register read");
    lv_obj_set_style_text_font(div1, &font_units_16, 0);
    lv_obj_set_style_text_color(div1, COL_TEXT, 0);

    dd_rfc   = form_dd(form_row("Function"), "FC03 holding\nFC04 input", 1);
    ta_raddr = form_ta(form_row("Address"), "0");
    ta_numeric(ta_raddr);
    dd_rcnt  = form_dd(form_row("Count"), "1\n2\n4\n8", 3);
    action_button(LV_SYMBOL_DOWNLOAD "  Read registers", reg_read_cb);

    lbl_rres = lv_label_create(panel);
    lv_label_set_text(lbl_rres, "");
    lv_obj_set_style_text_font(lbl_rres, &font_units_14, 0);

    lv_obj_t *div2 = lv_label_create(panel);
    lv_label_set_text(div2, "\nRegister write (FC06)");
    lv_obj_set_style_text_font(div2, &font_units_16, 0);
    lv_obj_set_style_text_color(div2, COL_TEXT, 0);

    ta_waddr = form_ta(form_row("Address"), "");
    ta_wval  = form_ta(form_row("Value"), "");
    ta_numeric(ta_waddr);
    ta_numeric(ta_wval);

    lv_obj_t *arow = form_row("Arm write (one write)");
    sw_arm = lv_switch_create(arow);
    lv_obj_align(sw_arm, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw_arm, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw_arm, COL_ACCENT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);

    action_button(LV_SYMBOL_UPLOAD "  Write register", reg_write_cb);

    lbl_wres = lv_label_create(panel);
    lv_label_set_text(lbl_wres, "Writes are verified against the card's"
                                " echo. Calibration presets will be added"
                                " with the card's register map.");
    lv_obj_set_style_text_font(lbl_wres, &font_units_12, 0);
    lv_obj_set_style_text_color(lbl_wres, COL_MUTED, 0);
}

/* ---- product theme selection --------------------------------------------- */

static lv_obj_t *btn_theme[UI_THEME_N];
static lv_obj_t *lbl_theme_res;

static void theme_mark(void)
{
    for (int i = 0; i < UI_THEME_N; i++) {
        bool act = (g_cfg.theme == i);
        lv_obj_set_style_border_width(btn_theme[i], act ? 3 : 1, 0);
        lv_obj_set_style_border_color(btn_theme[i],
            act ? lv_color_hex(ui_themes[i].accent)
                : lv_color_hex(ui_themes[i].border), 0);
    }
}

/* rebuild outside the button's own event context */
static void theme_apply_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    ui_reload();
}

static void theme_pick_cb(lv_event_t *e)
{
    g_cfg.theme = (int)(intptr_t)lv_event_get_user_data(e);
    config_save();
    event_log("CONFIG", "Theme changed to %s", ui_theme_names[g_cfg.theme]);
    lv_timer_t *t = lv_timer_create(theme_apply_timer_cb, 60, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void build_theme_form(void)
{
    for (int i = 0; i < UI_THEME_N; i++) {
        const ui_theme_t *t = &ui_themes[i];

        lv_obj_t *b = lv_button_create(panel);
        lv_obj_set_size(b, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(b, lv_color_hex(t->panel), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, theme_pick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        btn_theme[i] = b;

        lv_obj_t *nm = lv_label_create(b);
        lv_label_set_text(nm, ui_theme_names[i]);
        lv_obj_set_style_text_font(nm, &font_units_16, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(t->text), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);

        /* small preview dots: accent, text, muted on the theme panel */
        uint32_t dots[3] = { t->accent, t->text, t->muted };
        for (int d = 0; d < 3; d++) {
            lv_obj_t *dot = lv_obj_create(b);
            lv_obj_set_size(dot, 18, 18);
            lv_obj_set_style_radius(dot, 9, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(dots[d]), 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -14 - d * 26, 0);
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    lbl_theme_res = lv_label_create(panel);
    lv_label_set_text(lbl_theme_res,
        "Tap a theme - it applies to the whole recorder immediately.");
    lv_obj_set_style_text_font(lbl_theme_res, &font_units_12, 0);
    lv_obj_set_style_text_color(lbl_theme_res, COL_MUTED, 0);

    theme_mark();
}

/* ---- display colours: single fixed screen with colour swatches ---------- */

static lv_obj_t *dd_cmode, *lbl_colres;
static lv_obj_t *sw_single[UI_PALETTE_N];
static lv_obj_t *sw_ch[8][UI_PALETTE_N];
static lv_obj_t *single_lbl, *multi_lbl[8];
static lv_obj_t *btn_coltab[GROUP_COUNT], *col_tabbar;
static int sel_single, sel_col[CH_TOTAL], col_card;

static void swatch_mark(lv_obj_t **row, int selected)
{
    for (int i = 0; i < UI_PALETTE_N; i++) {
        lv_obj_set_style_border_width(row[i], i == selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(row[i],
            i == selected ? COL_TEXT : COL_BORDER, 0);
    }
}

static void swatch_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int row  = code >> 8;
    int idx  = code & 0xFF;

    if (row == 0xFF) {
        sel_single = idx;
        swatch_mark(sw_single, idx);
    } else {
        sel_col[col_card * CH_PER_GROUP + row] = idx;
        swatch_mark(sw_ch[row], idx);
    }
}

static void colors_tabs_update(void)
{
    int cards = chform_cards();
    for (int i = 0; i < cards; i++) {
        if (btn_coltab[i] == NULL) continue;
        bool act = (i == col_card);
        lv_obj_set_style_bg_color(btn_coltab[i],
                                  act ? COL_BG : COL_PANEL, 0);
        lv_obj_set_style_border_width(btn_coltab[i], act ? 2 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn_coltab[i], 0),
                                    act ? COL_TEXT : COL_MUTED, 0);
    }
    for (int r = 0; r < 8; r++) {
        lv_label_set_text_fmt(multi_lbl[r], "CH%d",
                              col_card * CH_PER_GROUP + r + 1);
        swatch_mark(sw_ch[r], sel_col[col_card * CH_PER_GROUP + r]);
    }
}

static void coltab_cb(lv_event_t *e)
{
    col_card = (int)(intptr_t)lv_event_get_user_data(e);
    colors_tabs_update();
}

static lv_obj_t *swatch(int x, int y, int row, int idx)
{
    lv_obj_t *b = lv_button_create(panel);
    lv_obj_set_size(b, 26, 26);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(ui_palette[idx]), 0);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_add_event_cb(b, swatch_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)((row << 8) | idx));
    return b;
}

static void colors_visibility(void)
{
    int single = (int)lv_dropdown_get_selected(dd_cmode);
    for (int i = 0; i < UI_PALETTE_N; i++) {
        if (single) lv_obj_remove_flag(sw_single[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(sw_single[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (single) lv_obj_remove_flag(single_lbl, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_add_flag(single_lbl, LV_OBJ_FLAG_HIDDEN);

    if (col_tabbar) {
        if (single) lv_obj_add_flag(col_tabbar, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(col_tabbar, LV_OBJ_FLAG_HIDDEN);
    }

    for (int r = 0; r < 8; r++) {
        if (single) lv_obj_add_flag(multi_lbl[r], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_remove_flag(multi_lbl[r], LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < UI_PALETTE_N; i++) {
            if (single) lv_obj_add_flag(sw_ch[r][i], LV_OBJ_FLAG_HIDDEN);
            else        lv_obj_remove_flag(sw_ch[r][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void colors_mode_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    colors_visibility();
}

static void colors_save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    g_cfg.color_mode   = (int)lv_dropdown_get_selected(dd_cmode);
    g_cfg.single_color = sel_single;
    for (int i = 0; i < CH_TOTAL; i++)
        g_cfg.ch_color[i] = sel_col[i];
    config_save();
    event_log("CONFIG", "Display colours saved");
    lv_label_set_text(lbl_colres, "Saved - applied to digital and bar views");
    lv_obj_set_style_text_color(lbl_colres, COL_ACCENT, 0);
}

static void build_colors_form(void)
{
    /* single fixed screen, no scrolling */
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    sel_single = g_cfg.single_color;
    if (sel_single < 0 || sel_single >= UI_PALETTE_N) sel_single = 0;
    for (int i = 0; i < CH_TOTAL; i++) {
        sel_col[i] = g_cfg.ch_color[i];
        if (sel_col[i] < 0 || sel_col[i] >= UI_PALETTE_N)
            sel_col[i] = i % CH_PER_GROUP;
    }
    if (col_card >= chform_cards()) col_card = 0;

    /* top row: mode + save */
    lv_obj_t *ml = lv_label_create(panel);
    lv_label_set_text(ml, "Colour mode");
    lv_obj_set_style_text_font(ml, &font_units_14, 0);
    lv_obj_set_style_text_color(ml, COL_MUTED, 0);
    lv_obj_set_pos(ml, 0, 12);

    dd_cmode = lv_dropdown_create(panel);
    lv_dropdown_set_options(dd_cmode,
                            "Multi-colour per channel\nSingle colour");
    lv_dropdown_set_selected(dd_cmode, (uint32_t)g_cfg.color_mode);
    lv_obj_set_size(dd_cmode, 250, 42);
    lv_obj_set_pos(dd_cmode, 110, 0);
    lv_obj_set_style_bg_color(dd_cmode, COL_BG, 0);
    lv_obj_set_style_text_color(dd_cmode, COL_TEXT, 0);
    lv_obj_set_style_border_color(dd_cmode, COL_BORDER, 0);
    lv_obj_t *mlst = lv_dropdown_get_list(dd_cmode);
    lv_obj_set_style_bg_color(mlst, COL_PANEL, 0);
    lv_obj_set_style_text_color(mlst, COL_TEXT, 0);
    lv_obj_add_event_cb(dd_cmode, colors_mode_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sv = lv_button_create(panel);
    lv_obj_set_size(sv, 190, 42);
    lv_obj_set_pos(sv, 534, 0);
    lv_obj_set_style_bg_color(sv, COL_ACCENT, 0);
    lv_obj_set_style_shadow_width(sv, 0, 0);
    lv_obj_add_event_cb(sv, colors_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *svl = lv_label_create(sv);
    lv_label_set_text(svl, LV_SYMBOL_SAVE "  Save colours");
    lv_obj_set_style_text_color(svl, COL_BG, 0);
    lv_obj_set_style_text_font(svl, &font_units_14, 0);
    lv_obj_center(svl);

    /* single colour swatch row */
    single_lbl = lv_label_create(panel);
    lv_label_set_text(single_lbl, "Colour");
    lv_obj_set_style_text_font(single_lbl, &font_units_14, 0);
    lv_obj_set_style_text_color(single_lbl, COL_MUTED, 0);
    lv_obj_set_pos(single_lbl, 0, 76);
    for (int i = 0; i < UI_PALETTE_N; i++)
        sw_single[i] = swatch(80 + i * 32, 70, 0xFF, i);
    swatch_mark(sw_single, sel_single);

    /* card tabs - hidden when only one card is fitted */
    int cards  = chform_cards();
    int rows_y = 56;
    col_tabbar = NULL;
    memset(btn_coltab, 0, sizeof(btn_coltab));
    if (cards > 1) {
        col_tabbar = tab_bar(panel, 52, 36);
        for (int i = 0; i < cards; i++) {
            char t[10];
            snprintf(t, sizeof(t), "Card %d", i + 1);
            btn_coltab[i] = tab_button(col_tabbar, t, coltab_cb, i);
        }
        rows_y = 96;
    }

    /* per-channel swatch rows: 4 left column, 4 right column */
    for (int r = 0; r < 8; r++) {
        int col  = r / 4;
        int rowy = rows_y + (r % 4) * 40;
        int x0   = col == 0 ? 0 : 390;

        multi_lbl[r] = lv_label_create(panel);
        lv_label_set_text_fmt(multi_lbl[r], "CH%d",
                              col_card * CH_PER_GROUP + r + 1);
        lv_obj_set_style_text_font(multi_lbl[r], &font_units_14, 0);
        lv_obj_set_style_text_color(multi_lbl[r], COL_MUTED, 0);
        lv_obj_set_pos(multi_lbl[r], x0, rowy + 6);

        for (int i = 0; i < UI_PALETTE_N; i++)
            sw_ch[r][i] = swatch(x0 + 50 + i * 32, rowy, r, i);
        swatch_mark(sw_ch[r], sel_col[col_card * CH_PER_GROUP + r]);
    }

    lbl_colres = lv_label_create(panel);
    lv_label_set_text(lbl_colres,
        "Individual colour per channel. Trend keeps its own colours.");
    lv_obj_set_style_text_font(lbl_colres, &font_units_12, 0);
    lv_obj_set_style_text_color(lbl_colres, COL_MUTED, 0);
    lv_obj_set_pos(lbl_colres, 0, rows_y + 170);

    if (cards > 1) colors_tabs_update();
    colors_visibility();
}

/* ---- event log viewer ----------------------------------------------------- */

static lv_obj_t *ev_table, *lbl_evday;
static int ev_day_off, ev_reload;

static void fill_events(void)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t ds = mktime(&tm) - (time_t)ev_day_off * 86400;
    struct tm dt = *localtime(&ds);

    lv_label_set_text_fmt(lbl_evday, "%s%02d-%02d-%04d",
                          ev_day_off == 0 ? "Today " : "",
                          dt.tm_mday, dt.tm_mon + 1, dt.tm_year + 1900);

    char path[64];
    snprintf(path, sizeof(path), "logs/events-%04d-%02d-%02d.csv",
             dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);

    /* keep the last 80 rows */
    #define EV_MAX 80
    static char ring[EV_MAX][220];
    int headi = 0, cnt = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char line[240];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "timestamp", 9)) continue;
            line[strcspn(line, "\r\n")] = 0;
            snprintf(ring[headi], sizeof(ring[0]), "%s", line);
            headi = (headi + 1) % EV_MAX;
            if (cnt < EV_MAX) cnt++;
        }
        fclose(f);
    }

    lv_table_set_row_count(ev_table, (uint32_t)(cnt + 1));
    lv_table_set_cell_value(ev_table, 0, 0, "Time");
    lv_table_set_cell_value(ev_table, 0, 1, "Category");
    lv_table_set_cell_value(ev_table, 0, 2, "Description");
    lv_table_set_cell_value(ev_table, 0, 3, "User");

    for (int i = 0; i < cnt; i++) {
        int idx = (headi - 1 - i + EV_MAX) % EV_MAX;
        char ts[24] = "", cat[16] = "", desc[200] = "", usr[20] = "";
        sscanf(ring[idx], "%23[^,],%15[^,],%199[^,],%19[^\n]",
               ts, cat, desc, usr);
        lv_table_set_cell_value(ev_table, i + 1, 0,
                                strlen(ts) > 11 ? ts + 11 : ts);
        lv_table_set_cell_value(ev_table, i + 1, 1, cat);
        lv_table_set_cell_value(ev_table, i + 1, 2, desc);
        lv_table_set_cell_value(ev_table, i + 1, 3,
                                usr[0] ? usr : "-");
    }

    if (cnt == 0) {
        lv_table_set_row_count(ev_table, 2);
        lv_table_set_cell_value(ev_table, 1, 0, "-");
        lv_table_set_cell_value(ev_table, 1, 1, "-");
        lv_table_set_cell_value(ev_table, 1, 2, "No events this day");
        lv_table_set_cell_value(ev_table, 1, 3, "-");
    }
}

static void evprev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ev_day_off++;
    fill_events();
}

static void evnext_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (ev_day_off > 0) { ev_day_off--; fill_events(); }
}

static void build_events_form(void)
{
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(panel, 8, 0);

    lv_obj_t *bp = lv_button_create(panel);
    lv_obj_set_size(bp, 40, 34);
    lv_obj_set_pos(bp, 0, 0);
    lv_obj_set_style_bg_color(bp, COL_BG, 0);
    lv_obj_set_style_border_color(bp, COL_BORDER, 0);
    lv_obj_set_style_border_width(bp, 1, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_add_event_cb(bp, evprev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bpl = lv_label_create(bp);
    lv_label_set_text(bpl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bpl, COL_TEXT, 0);
    lv_obj_center(bpl);

    lbl_evday = lv_label_create(panel);
    lv_obj_set_style_text_font(lbl_evday, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_evday, COL_TEXT, 0);
    lv_obj_set_pos(lbl_evday, 52, 8);
    lv_obj_set_width(lbl_evday, 150);

    lv_obj_t *bn = lv_button_create(panel);
    lv_obj_set_size(bn, 40, 34);
    lv_obj_set_pos(bn, 208, 0);
    lv_obj_set_style_bg_color(bn, COL_BG, 0);
    lv_obj_set_style_border_color(bn, COL_BORDER, 0);
    lv_obj_set_style_border_width(bn, 1, 0);
    lv_obj_set_style_shadow_width(bn, 0, 0);
    lv_obj_add_event_cb(bn, evnext_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bnl = lv_label_create(bn);
    lv_label_set_text(bnl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(bnl, COL_TEXT, 0);
    lv_obj_center(bnl);

    ev_table = lv_table_create(panel);
    lv_obj_set_size(ev_table, LV_PCT(100), 480 - 40 - 56 - 52 - 60);
    lv_obj_set_pos(ev_table, 0, 42);
    lv_obj_set_style_bg_color(ev_table, COL_PANEL, 0);
    lv_obj_set_style_border_color(ev_table, COL_BORDER, 0);
    lv_obj_set_style_bg_color(ev_table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(ev_table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(ev_table, &font_units_12,
                               LV_PART_ITEMS);
    lv_obj_set_style_border_color(ev_table, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(ev_table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(ev_table, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(ev_table, 8, LV_PART_ITEMS);

    lv_table_set_column_count(ev_table, 3);
    lv_table_set_column_width(ev_table, 0, 92);
    lv_table_set_column_width(ev_table, 1, 104);
    lv_table_set_column_width(ev_table, 2, 440);
    lv_table_set_column_width(ev_table, 3, 100);

    ev_day_off = 0;
    ev_reload = 0;
    fill_events();
}

/* ---- About: animated recorder watermark --------------------------------
 * A faint image of what the product does: a strip chart recording two
 * live pen traces, a classic circular-chart dial with a turning pen
 * arm, and a set of breathing channel bars. Animations are attached to
 * their objects, so LVGL removes them when the page is left; the chart
 * feed timer deletes itself when the chart is gone. */

static void wm_anim_h(void *obj, int32_t v)   { lv_obj_set_height(obj, v); }
static void wm_anim_rot(void *obj, int32_t v)
{
    lv_obj_set_style_transform_rotation(obj, v, 0);
}

static lv_obj_t *wm_chart;
static lv_chart_series_t *wm_s1, *wm_s2;
static lv_timer_t *wm_timer;

/* delete a page-scoped timer when its page root is torn down; the timer
 * pointer lives in a static, passed by address as user_data. Guards
 * against re-entering a page and orphaning (or duplicating) the timer. */
static void timer_cleanup_cb(lv_event_t *e)
{
    lv_timer_t **slot = (lv_timer_t **)lv_event_get_user_data(e);
    if (slot && *slot) { lv_timer_delete(*slot); *slot = NULL; }
}

static void wm_chart_tick(lv_timer_t *tmr)
{
    if (!wm_chart || !lv_obj_is_valid(wm_chart)) {
        lv_timer_delete(tmr);
        wm_timer = NULL;
        wm_chart = NULL;
        return;
    }
    static int ph;
    ph++;
    int v1 = 58 + (int)(26.0f * sinf((float)ph * 0.11f))
                + (rand() % 5 - 2);
    int v2 = 38 + (int)(15.0f * sinf((float)ph * 0.06f + 2.1f))
                + (rand() % 5 - 2);
    lv_chart_set_next_value(wm_chart, wm_s1, v1);
    lv_chart_set_next_value(wm_chart, wm_s2, v2);
}

static void wm_anim(lv_obj_t *obj, int32_t lo, int32_t hi, uint32_t dur,
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

static void build_about(void)
{
    lv_obj_set_layout(panel, LV_LAYOUT_NONE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, timer_cleanup_cb, LV_EVENT_DELETE, &wm_timer);

    /* --- watermark scene (built first = stays behind the text) --- */

    /* strip chart recording two live pen traces, like the trend view */
    wm_chart = lv_chart_create(panel);
    lv_obj_set_size(wm_chart, 520, 240);
    lv_obj_set_pos(wm_chart, 96, 34);
    lv_chart_set_type(wm_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(wm_chart, 90);
    lv_chart_set_range(wm_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_update_mode(wm_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(wm_chart, 5, 8);
    lv_obj_set_style_bg_opa(wm_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wm_chart, 0, 0);
    lv_obj_set_style_line_color(wm_chart, COL_MUTED, LV_PART_MAIN);
    lv_obj_set_style_line_opa(wm_chart, 35, LV_PART_MAIN);
    lv_obj_set_style_line_opa(wm_chart, 95, LV_PART_ITEMS);
    lv_obj_set_style_line_width(wm_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(wm_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(wm_chart, LV_OBJ_FLAG_CLICKABLE);

    wm_s1 = lv_chart_add_series(wm_chart, COL_ACCENT,
                                LV_CHART_AXIS_PRIMARY_Y);
    wm_s2 = lv_chart_add_series(wm_chart, COL_MUTED,
                                LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(wm_chart, wm_s1, 58);
    lv_chart_set_all_value(wm_chart, wm_s2, 38);
    wm_timer = lv_timer_create(wm_chart_tick, 90, NULL);

    /* classic circular-chart dial with a turning pen arm */
    for (int r = 0; r < 3; r++) {
        lv_obj_t *ring = lv_arc_create(panel);
        int d = 130 - r * 40;
        lv_obj_set_size(ring, d, d);
        lv_obj_set_pos(ring, 620 + (130 - d) / 2, 140 + (130 - d) / 2);
        lv_arc_set_bg_angles(ring, 0, 360);
        lv_arc_set_value(ring, 0);
        lv_obj_set_style_arc_color(ring, COL_MUTED, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ring, 50, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring, 1, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ring, 0, LV_PART_INDICATOR);
        lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *pen = lv_obj_create(panel);
    lv_obj_set_size(pen, 3, 62);
    lv_obj_set_pos(pen, 684, 143);       /* bottom end = dial centre */
    lv_obj_set_style_bg_color(pen, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(pen, 120, 0);
    lv_obj_set_style_border_width(pen, 0, 0);
    lv_obj_set_style_radius(pen, 2, 0);
    lv_obj_set_style_transform_pivot_x(pen, 1, 0);
    lv_obj_set_style_transform_pivot_y(pen, 62, 0);
    lv_obj_remove_flag(pen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    wm_anim(pen, 0, 3600, 14000, 0, 0, wm_anim_rot);

    /* breathing channel bars, like the bar-graph view (bottom-aligned
     * inside a box so they grow upward from a fixed base line) */
    lv_obj_t *barbox = lv_obj_create(panel);
    lv_obj_set_size(barbox, 92, 118);
    lv_obj_set_pos(barbox, 14, 146);
    lv_obj_set_style_bg_opa(barbox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(barbox, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(barbox, COL_MUTED, 0);
    lv_obj_set_style_border_opa(barbox, 60, 0);
    lv_obj_set_style_border_width(barbox, 2, 0);
    lv_obj_set_style_pad_all(barbox, 0, 0);
    lv_obj_remove_flag(barbox, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_CLICKABLE);

    static const uint32_t bar_dur[4] = { 2100, 2900, 2500, 3300 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(barbox);
        lv_obj_set_size(bar, 14, 40);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 8 + i * 20, -2);
        lv_obj_set_style_bg_color(bar, i % 2 ? COL_MUTED : COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(bar, 70, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICKABLE);
        wm_anim(bar, 28, 106, bar_dur[i], (uint32_t)i * 500, 1,
                wm_anim_h);
    }

    /* --- product info on top of the watermark --- */

    lv_obj_t *t = lv_label_create(panel);
    lv_label_set_text(t, g_cfg.brand);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, COL_TEXT, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *m = lv_label_create(panel);
    lv_label_set_text(m, g_cfg.model);
    lv_obj_set_style_text_font(m, &font_units_16, 0);
    lv_obj_set_style_text_color(m, COL_ACCENT, 0);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *l = lv_label_create(panel);
    lv_label_set_text(l,
        "40-channel paperless recorder\n"
        "8-channel universal input cards\n\n"
        "Firmware version: " FW_VERSION "\n"
        "Support: contact your supplier");
    lv_obj_set_style_text_color(l, COL_MUTED, 0);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 100);
}

/* ---- Software update (OTA from GitHub Releases) ------------------------ */

static lv_obj_t  *upd_status, *upd_btn_check, *upd_btn_install;
static lv_timer_t *upd_timer;

static void upd_ui_sync(void)
{
    upd_state_t s = update_state();
    lv_label_set_text(upd_status, update_message());
    lv_obj_set_style_text_color(upd_status,
        s == UPD_ERROR   ? COL_ALARM_TXT :
        s == UPD_NEWER   ? COL_ACCENT    :
        s == UPD_UPTODATE || s == UPD_RESTARTING ? COL_ACCENT
                         : COL_MUTED, 0);

    if (s == UPD_NEWER)
        lv_obj_remove_state(upd_btn_install, LV_STATE_DISABLED);
    else
        lv_obj_add_state(upd_btn_install, LV_STATE_DISABLED);

    if (s == UPD_CHECKING || s == UPD_DOWNLOADING ||
        s == UPD_RESTARTING)
        lv_obj_add_state(upd_btn_check, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(upd_btn_check, LV_STATE_DISABLED);
}

static void upd_poll(lv_timer_t *t)
{
    if (!upd_status || !lv_obj_is_valid(upd_status)) {
        lv_timer_delete(t);
        upd_timer = NULL;
        return;
    }
    upd_ui_sync();
}

static void upd_check_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    update_check_async();
    upd_ui_sync();
}

static void upd_install_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    update_apply_async();
    upd_ui_sync();
}

static void build_update_form(void)
{
    lv_obj_add_event_cb(panel, timer_cleanup_cb, LV_EVENT_DELETE,
                        &upd_timer);

    lv_obj_t *row = form_row("Installed version");
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, FW_VERSION);
    lv_obj_set_style_text_font(v, &font_units_16, 0);
    lv_obj_set_style_text_color(v, COL_TEXT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -8, 0);

    row = form_row("Update server");
    lv_obj_t *sv = lv_label_create(row);
    lv_label_set_text_fmt(sv, "%s%s",
        g_cfg.update_repo[0] ? "github.com/" : "not configured",
        g_cfg.update_repo);
    lv_obj_set_style_text_font(sv, &font_units_14, 0);
    lv_obj_set_style_text_color(sv, COL_MUTED, 0);
    lv_obj_align(sv, LV_ALIGN_RIGHT_MID, -8, 0);

    /* action row: check + install */
    lv_obj_t *btns = lv_obj_create(panel);
    lv_obj_set_size(btns, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    upd_btn_check = lv_button_create(btns);
    lv_obj_set_size(upd_btn_check, 220, 46);
    lv_obj_align(upd_btn_check, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(upd_btn_check, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(upd_btn_check, COL_PANEL,
                              LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(upd_btn_check, 0, 0);
    lv_obj_add_event_cb(upd_btn_check, upd_check_cb, LV_EVENT_CLICKED,
                        NULL);
    lv_obj_t *cl = lv_label_create(upd_btn_check);
    lv_label_set_text(cl, LV_SYMBOL_REFRESH "  Check for update");
    lv_obj_set_style_text_font(cl, &font_units_14, 0);
    lv_obj_set_style_text_color(cl, COL_BG, 0);
    lv_obj_set_style_text_color(cl, COL_MUTED, LV_STATE_DISABLED);
    lv_obj_center(cl);

    upd_btn_install = lv_button_create(btns);
    lv_obj_set_size(upd_btn_install, 220, 46);
    lv_obj_align(upd_btn_install, LV_ALIGN_LEFT_MID, 236, 0);
    lv_obj_set_style_bg_color(upd_btn_install, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(upd_btn_install, COL_PANEL,
                              LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(upd_btn_install, 0, 0);
    lv_obj_add_state(upd_btn_install, LV_STATE_DISABLED);
    lv_obj_add_event_cb(upd_btn_install, upd_install_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *il = lv_label_create(upd_btn_install);
    lv_label_set_text(il, LV_SYMBOL_DOWNLOAD "  Install update");
    lv_obj_set_style_text_font(il, &font_units_14, 0);
    lv_obj_set_style_text_color(il, COL_BG, 0);
    lv_obj_set_style_text_color(il, COL_MUTED, LV_STATE_DISABLED);
    lv_obj_center(il);

    upd_status = lv_label_create(panel);
    lv_label_set_text(upd_status, "");
    lv_obj_set_style_text_font(upd_status, &font_units_14, 0);
    lv_obj_set_style_text_color(upd_status, COL_MUTED, 0);
    lv_obj_set_width(upd_status, LV_PCT(100));
    lv_label_set_long_mode(upd_status, LV_LABEL_LONG_WRAP);

    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note,
        "Updates are downloaded from the configured GitHub repository"
        " (newest release, asset 'recorder_ui'). The previous version"
        " is kept as recorder_ui.bak for rollback. The recorder"
        " restarts automatically after installing.");
    lv_obj_set_style_text_color(note, COL_MUTED, 0);
    lv_obj_set_style_text_font(note, &font_units_12, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    upd_ui_sync();
    upd_timer = lv_timer_create(upd_poll, 300, NULL);
}

/* ---- page scaffolding ---------------------------------------------------- */

static int back_to_svc;   /* 0 main, 1 Factory settings, 2 Display,
                           * 3 Network */

static void back_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (back_to_svc == 1)      show_svc_list();
    else if (back_to_svc == 2) show_disp_list();
    else if (back_to_svc == 3) show_net_list();
    else                       show_main_list();
}

static void page_open(const char *icon, const char *title, int svc_back)
{
    back_to_svc = svc_back;
    lv_obj_clean(root);
    pin_dlg = NULL;    /* dialogs (if any) died with the clean */
    rst_dlg = NULL;
    den_dlg = NULL;

    /* header with back button */
    lv_obj_t *hdr = lv_obj_create(root);
    lv_obj_set_size(hdr, LV_PCT(100), 52);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, COL_BORDER, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_button_create(hdr);
    lv_obj_set_size(back, 90, 42);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, COL_BG, 0);
    lv_obj_set_style_border_color(back, COL_BORDER, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_font(bl, &font_units_14, 0);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);

    lv_obj_t *tlbl = lv_label_create(hdr);
    lv_label_set_text_fmt(tlbl, "%s   %s", icon, title);
    lv_obj_set_style_text_font(tlbl, &font_units_16, 0);
    lv_obj_set_style_text_color(tlbl, COL_TEXT, 0);
    lv_obj_align(tlbl, LV_ALIGN_CENTER, 0, 0);

    page_hdr = hdr;     /* forms may add a Save button top right */
    save_btn = NULL;
    form_loading = 0;

    /* scrollable form panel below the header */
    panel = lv_obj_create(root);
    lv_obj_set_size(panel, LV_PCT(100), 480 - 40 - 56 - 52);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(panel, COL_BG, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_hor(panel, 20, 0);
    lv_obj_set_style_pad_ver(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);

    /* keyboard overlay (hidden until a field is focused) */
    kb = lv_keyboard_create(root);
    lv_obj_set_size(kb, LV_PCT(100), 190);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb, COL_PANEL, 0);
    lv_obj_set_style_bg_color(kb, COL_BG, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &font_units_16, LV_PART_ITEMS);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, unit_map, unit_ctrl);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    /* typing preview bar directly above the keyboard */
    kb_prev = lv_obj_create(root);
    lv_obj_set_size(kb_prev, LV_PCT(100), 48);
    lv_obj_align(kb_prev, LV_ALIGN_BOTTOM_MID, 0, -190);
    lv_obj_set_style_bg_color(kb_prev, COL_PANEL, 0);
    lv_obj_set_style_border_color(kb_prev, COL_ACCENT, 0);
    lv_obj_set_style_border_side(kb_prev, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(kb_prev, 2, 0);
    lv_obj_set_style_radius(kb_prev, 0, 0);
    lv_obj_remove_flag(kb_prev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(kb_prev, LV_OBJ_FLAG_HIDDEN);

    kb_prev_lbl = lv_label_create(kb_prev);
    lv_label_set_text(kb_prev_lbl, "");
    lv_obj_set_style_text_font(kb_prev_lbl, &font_units_28, 0);
    lv_obj_set_style_text_color(kb_prev_lbl, COL_TEXT, 0);
    lv_obj_center(kb_prev_lbl);
}

/* ---- item callbacks ------------------------------------------------------- */

static void build_colors_form(void);
static void build_events_form(void);
static void build_network_form(void);
static void build_wifi_form(void);
static void build_diag_form(void);

/* ---- 21 CFR login screen ----------------------------------------------- */

static lv_obj_t *login_dd, *login_ta, *login_msg;
static int login_map[CFR_USERS];   /* dropdown row -> user slot */
static int login_n;

static void login_key_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *txt = lv_buttonmatrix_get_button_text(bm, id);
    if (!txt) return;

    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        lv_textarea_delete_char(login_ta);
        return;
    }
    if (!strcmp(txt, LV_SYMBOL_CLOSE)) {
        lv_textarea_set_text(login_ta, "");
        return;
    }
    if (!strcmp(txt, LV_SYMBOL_OK)) {
        int row = (int)lv_dropdown_get_selected(login_dd);
        int idx = (row >= 0 && row < login_n) ? login_map[row] : -1;
        int rc = cfr_login(idx, lv_textarea_get_text(login_ta));
        if (rc == 0) {
            show_main_list();
        } else {
            lv_textarea_set_text(login_ta, "");
            lv_label_set_text(login_msg, rc == -2
                ? "PIN expired - ask an administrator to reset it"
                : "Wrong PIN, or account locked - try again");
            lv_obj_set_style_text_color(login_msg, COL_ALARM_TXT, 0);
        }
        return;
    }
    lv_textarea_add_text(login_ta, txt);
}

static void build_login(void)
{
    static const char *login_kmap[] = {
        "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
        LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE, "\n",
        LV_SYMBOL_OK, ""
    };

    lv_obj_clean(root);
    pin_dlg = NULL;
    rst_dlg = NULL;

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_size(box, 330, 380);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_border_color(box, COL_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_KEYBOARD "  Sign in");
    lv_obj_set_style_text_font(title, &font_units_16, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* active users only */
    char opts[CFR_USERS * 20] = "";
    login_n = 0;
    for (int i = 0; i < CFR_USERS; i++) {
        if (!g_cfg.users[i].active || !g_cfg.users[i].name[0]) continue;
        if (login_n) strcat(opts, "\n");
        strcat(opts, g_cfg.users[i].name);
        login_map[login_n++] = i;
    }
    if (login_n == 0) { strcpy(opts, "SUPER ADMIN"); login_map[0] = 0;
                        login_n = 1; }

    login_dd = lv_dropdown_create(box);
    lv_dropdown_set_options(login_dd, opts);
    lv_obj_set_size(login_dd, 200, 40);
    lv_obj_align(login_dd, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_bg_color(login_dd, COL_BG, 0);
    lv_obj_set_style_text_color(login_dd, COL_TEXT, 0);
    lv_obj_set_style_border_color(login_dd, COL_BORDER, 0);
    lv_obj_t *dl = lv_dropdown_get_list(login_dd);
    lv_obj_set_style_bg_color(dl, COL_PANEL, 0);
    lv_obj_set_style_text_color(dl, COL_TEXT, 0);

    login_ta = lv_textarea_create(box);
    lv_textarea_set_one_line(login_ta, true);
    lv_textarea_set_password_mode(login_ta, true);
    lv_textarea_set_max_length(login_ta, 10);
    lv_obj_set_size(login_ta, 200, 40);
    lv_obj_align(login_ta, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_color(login_ta, COL_BG, 0);
    lv_obj_set_style_text_color(login_ta, COL_TEXT, 0);
    lv_obj_set_style_border_color(login_ta, COL_BORDER, 0);
    lv_obj_set_style_text_align(login_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(login_ta, LV_OBJ_FLAG_CLICKABLE);

    login_msg = lv_label_create(box);
    lv_label_set_text(login_msg, "Select your name and enter the PIN");
    lv_obj_set_style_text_font(login_msg, &font_units_12, 0);
    lv_obj_set_style_text_color(login_msg, COL_MUTED, 0);
    lv_obj_align(login_msg, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *bm = lv_buttonmatrix_create(box);
    lv_buttonmatrix_set_map(bm, login_kmap);
    lv_obj_set_size(bm, 290, 200);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_bg_color(bm, COL_BG, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(bm, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &font_units_16, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, login_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- access-denied notice (21 CFR role gate) --------------------------- */

static void den_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (den_dlg) { lv_obj_delete(den_dlg); den_dlg = NULL; }
}

static void access_denied(const char *level)
{
    den_dlg = lv_obj_create(root);
    lv_obj_set_size(den_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(den_dlg, COL_BG, 0);
    lv_obj_set_style_bg_opa(den_dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(den_dlg, 0, 0);
    lv_obj_remove_flag(den_dlg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(den_dlg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(den_dlg, den_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(den_dlg);
    lv_obj_set_size(box, 360, 130);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_border_color(box, COL_ALARM, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_WARNING "  Access denied");
    lv_obj_set_style_text_font(title, &font_units_16, 0);
    lv_obj_set_style_text_color(title, COL_ALARM_TXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text_fmt(msg, "%s level is required for this function."
                          "\nTap anywhere to continue.", level);
    lv_obj_set_style_text_font(msg, &font_units_12, 0);
    lv_obj_set_style_text_color(msg, COL_MUTED, 0);
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_BOTTOM_MID, 0, -10);
}

/* ---- Factory settings PIN gate ----------------------------------------- */

static lv_obj_t *pin_ta, *pin_msg;

static void pin_key_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *txt = lv_buttonmatrix_get_button_text(bm, id);
    if (!txt) return;

    if (!strcmp(txt, LV_SYMBOL_BACKSPACE)) {
        lv_textarea_delete_char(pin_ta);
        return;
    }
    if (!strcmp(txt, LV_SYMBOL_CLOSE)) {
        lv_obj_delete(pin_dlg);
        pin_dlg = NULL;
        return;
    }
    if (!strcmp(txt, LV_SYMBOL_OK)) {
        if (!strcmp(lv_textarea_get_text(pin_ta), g_cfg.factory_pin)) {
            lv_obj_delete(pin_dlg);
            pin_dlg = NULL;
            event_log("SYSTEM", "Factory settings unlocked");
            show_svc_list();
        } else {
            event_log("SYSTEM", "Factory settings: wrong password");
            lv_textarea_set_text(pin_ta, "");
            lv_label_set_text(pin_msg, "Wrong password - try again");
            lv_obj_set_style_text_color(pin_msg, COL_ALARM_TXT, 0);
        }
        return;
    }
    lv_textarea_add_text(pin_ta, txt);
}

static void pin_dialog_open(void)
{
    static const char *pin_map[] = {
        "1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
        LV_SYMBOL_CLOSE, "0", LV_SYMBOL_BACKSPACE, "\n",
        LV_SYMBOL_OK, ""
    };

    pin_dlg = lv_obj_create(root);
    lv_obj_set_size(pin_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(pin_dlg, COL_BG, 0);
    lv_obj_set_style_bg_opa(pin_dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(pin_dlg, 0, 0);
    lv_obj_set_style_radius(pin_dlg, 0, 0);
    lv_obj_remove_flag(pin_dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(pin_dlg);
    lv_obj_set_size(box, 300, 372);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_border_color(box, COL_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Service password");
    lv_obj_set_style_text_font(title, &font_units_16, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    pin_ta = lv_textarea_create(box);
    lv_textarea_set_one_line(pin_ta, true);
    lv_textarea_set_password_mode(pin_ta, true);
    lv_textarea_set_max_length(pin_ta, 10);
    lv_obj_set_size(pin_ta, 200, 42);
    lv_obj_align(pin_ta, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(pin_ta, COL_BG, 0);
    lv_obj_set_style_text_color(pin_ta, COL_TEXT, 0);
    lv_obj_set_style_border_color(pin_ta, COL_BORDER, 0);
    lv_obj_set_style_text_align(pin_ta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(pin_ta, LV_OBJ_FLAG_CLICKABLE);

    pin_msg = lv_label_create(box);
    lv_label_set_text(pin_msg, "Enter the service password");
    lv_obj_set_style_text_font(pin_msg, &font_units_12, 0);
    lv_obj_set_style_text_color(pin_msg, COL_MUTED, 0);
    lv_obj_align(pin_msg, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *bm = lv_buttonmatrix_create(box);
    lv_buttonmatrix_set_map(bm, pin_map);
    lv_obj_set_size(bm, 268, 230);
    lv_obj_align(bm, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_pad_all(bm, 0, 0);
    lv_obj_set_style_bg_color(bm, COL_BG, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(bm, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(bm, &font_units_16, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, pin_key_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void main_item_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    main_sec_t s = (main_sec_t)main_map[row];

    /* 21 CFR role gate: operator views/exports/acks; supervisor adds
     * configuration; administrator adds accounts; only the super
     * admin reaches Factory settings */
    if (cfr_on()) {
        if (s == M_CARDSVC && cfr_role() < ROLE_SUPERADMIN) {
            access_denied("Super admin");
            return;
        }
        if (s == M_ACCOUNTS && cfr_role() < ROLE_ADMIN) {
            access_denied("Admin");
            return;
        }
        if ((s == M_CHANNEL || s == M_LOGGING || s == M_NETWORK) &&
            cfr_role() < ROLE_SUPERVISOR) {
            access_denied("Supervisor");
            return;
        }
    }

    if (s == M_CARDSVC) {
        /* logged-in administrator already authenticated; otherwise
         * the service PIN applies as before */
        if (cfr_on()) show_svc_list();
        else          pin_dialog_open();
        return;
    }
    if (s == M_DISPLAY)  { show_disp_list(); return; }
    if (s == M_NETWORK)  { show_net_list();  return; }

    page_open(main_icons[s], main_names[s], 0);
    switch (s) {
    case M_CHANNEL:  build_channel_form();  break;
    case M_LOGGING:  build_logging_form();  break;
    case M_EXPORT:   build_export_form();   break;
    case M_EVENTS:   build_events_form();   break;
    case M_ACCOUNTS: build_accounts_form(); break;
    case M_ABOUT:    build_about();         break;
    default: break;
    }
}

static void build_theme_form(void);

static void disp_item_cb(lv_event_t *e)
{
    disp_sec_t s = (disp_sec_t)(intptr_t)lv_event_get_user_data(e);
    page_open(disp_icons[s], disp_names[s], 2);
    if (s == D_COLORS) build_colors_form();
    if (s == D_THEME)  build_theme_form();
}

/* hardware-restart confirmation over the Factory settings list */
static void rst_yes_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    recorder_restart();      /* does not return */
}

static void rst_no_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (rst_dlg) { lv_obj_delete(rst_dlg); rst_dlg = NULL; }
}

static void restart_confirm_open(void)
{
    rst_dlg = lv_obj_create(root);
    lv_obj_set_size(rst_dlg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(rst_dlg, COL_BG, 0);
    lv_obj_set_style_bg_opa(rst_dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(rst_dlg, 0, 0);
    lv_obj_set_style_radius(rst_dlg, 0, 0);
    lv_obj_remove_flag(rst_dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(rst_dlg);
    lv_obj_set_size(box, 360, 190);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_border_color(box, COL_BORDER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, LV_SYMBOL_POWER "  Restart recorder?");
    lv_obj_set_style_text_font(title, &font_units_16, 0);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *msg = lv_label_create(box);
    lv_label_set_text(msg, "The recorder will reboot. Logging resumes"
                           " automatically after start-up.");
    lv_obj_set_style_text_font(msg, &font_units_12, 0);
    lv_obj_set_style_text_color(msg, COL_MUTED, 0);
    lv_obj_set_width(msg, LV_PCT(100));
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *no = lv_button_create(box);
    lv_obj_set_size(no, 140, 46);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(no, COL_BG, 0);
    lv_obj_set_style_border_color(no, COL_BORDER, 0);
    lv_obj_set_style_border_width(no, 1, 0);
    lv_obj_set_style_shadow_width(no, 0, 0);
    lv_obj_add_event_cb(no, rst_no_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(no);
    lv_label_set_text(nl, "Cancel");
    lv_obj_set_style_text_color(nl, COL_TEXT, 0);
    lv_obj_set_style_text_font(nl, &font_units_14, 0);
    lv_obj_center(nl);

    lv_obj_t *yes = lv_button_create(box);
    lv_obj_set_size(yes, 140, 46);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, COL_ALARM, 0);
    lv_obj_set_style_shadow_width(yes, 0, 0);
    lv_obj_add_event_cb(yes, rst_yes_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *yl = lv_label_create(yes);
    lv_label_set_text(yl, LV_SYMBOL_POWER "  Restart");
    lv_obj_set_style_text_color(yl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(yl, &font_units_14, 0);
    lv_obj_center(yl);
}

static void svc_item_cb(lv_event_t *e)
{
    svc_sec_t s = (svc_sec_t)(intptr_t)lv_event_get_user_data(e);
    if (s == S_RESTART) { restart_confirm_open(); return; }

    page_open(svc_icons[s], svc_names[s], 1);
    switch (s) {
    case S_COMM:    build_comm_form();    break;
    case S_CAL:     build_cal_form();     break;
    case S_CARDCFG: build_cardcfg_form(); break;
    case S_REGS:     build_regs_form();     break;
    case S_DIAG:     build_diag_form();     break;
    case S_CFR:      build_cfr_form();      break;
    case S_UPDATE:   build_update_form();   break;
    default: break;
    }
}

static void net_item_cb(lv_event_t *e)
{
    net_sec_t s = (net_sec_t)(intptr_t)lv_event_get_user_data(e);
    page_open(net_icons[s], net_names[s], 3);
    if (s == N_ETH)  build_network_form();
    if (s == N_WIFI) build_wifi_form();
}

/* generic full-width menu list; hdr_title != NULL adds a back header */
static void build_menu_list(const char *hdr_title, const char **names,
                            const char **icons, int count,
                            lv_event_cb_t cb)
{
    lv_obj_clean(root);
    pin_dlg = NULL;    /* dialogs (if any) died with the clean */
    rst_dlg = NULL;
    den_dlg = NULL;
    int top = 0;

    if (hdr_title) {
        lv_obj_t *hdr = lv_obj_create(root);
        lv_obj_set_size(hdr, LV_PCT(100), 52);
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(hdr, COL_PANEL, 0);
        lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(hdr, 1, 0);
        lv_obj_set_style_border_color(hdr, COL_BORDER, 0);
        lv_obj_set_style_radius(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 4, 0);
        lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *back = lv_button_create(hdr);
        lv_obj_set_size(back, 90, 42);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_bg_color(back, COL_BG, 0);
        lv_obj_set_style_border_color(back, COL_BORDER, 0);
        lv_obj_set_style_border_width(back, 1, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        back_to_svc = 0;
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *bl = lv_label_create(back);
        lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
        lv_obj_set_style_text_font(bl, &font_units_14, 0);
        lv_obj_set_style_text_color(bl, COL_TEXT, 0);
        lv_obj_center(bl);

        lv_obj_t *tl = lv_label_create(hdr);
        lv_label_set_text_fmt(tl, LV_SYMBOL_SETTINGS "   %s", hdr_title);
        lv_obj_set_style_text_font(tl, &font_units_16, 0);
        lv_obj_set_style_text_color(tl, COL_TEXT, 0);
        lv_obj_align(tl, LV_ALIGN_CENTER, 0, 0);
        top = 52;
    }

    lv_obj_t *list = lv_obj_create(root);
    lv_obj_set_size(list, LV_PCT(100), (480 - 40 - 56) - top);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_color(list, COL_BG, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 60, 0);
    lv_obj_set_style_pad_ver(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, count >= 8 ? 3 : 4, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < count; i++) {
        lv_obj_t *b = lv_button_create(list);
        lv_obj_set_size(b, LV_PCT(100), count >= 8 ? 36 : 40);
        lv_obj_set_style_bg_color(b, COL_PANEL, 0);
        lv_obj_set_style_border_color(b, COL_BORDER, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_label_create(b);
        lv_label_set_text(ic, icons[i]);
        lv_obj_set_style_text_font(ic, &font_units_16, 0);
        lv_obj_set_style_text_color(ic, COL_ACCENT, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t *nm = lv_label_create(b);
        lv_label_set_text(nm, names[i]);
        lv_obj_set_style_text_font(nm, &font_units_16, 0);
        lv_obj_set_style_text_color(nm, COL_TEXT, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 56, 0);

        lv_obj_t *chv = lv_label_create(b);
        lv_label_set_text(chv, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(chv, COL_MUTED, 0);
        lv_obj_align(chv, LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

static void show_svc_list(void)
{
    build_menu_list("Factory settings", svc_names, svc_icons,
                    S_COUNT, svc_item_cb);
}

static void show_net_list(void)
{
    build_menu_list("Network", net_names, net_icons,
                    N_COUNT, net_item_cb);
}

static void show_disp_list(void)
{
    build_menu_list("Display", disp_names, disp_icons,
                    D_COUNT, disp_item_cb);
}

static void show_main_list(void)
{
    /* Account manager is only offered while 21 CFR mode is on */
    static const char *names[M_COUNT];
    static const char *icons[M_COUNT];
    int n = 0;
    for (int s = 0; s < M_COUNT; s++) {
        if (s == M_ACCOUNTS && !cfr_on()) continue;
        names[n] = main_names[s];
        icons[n] = main_icons[s];
        main_map[n] = s;
        n++;
    }
    build_menu_list(NULL, names, icons, n, main_item_cb);
}

void scr_menu_build(lv_obj_t *parent)
{
    root = parent;
    if (cfr_on() && cfr_logged_idx() < 0)
        build_login();       /* 21 CFR: the menu starts at the sign-in */
    else
        show_main_list();
}

void scr_menu_refresh(void)
{
    /* today's event log follows new entries every 10 s */
    if (ev_table && lv_obj_is_valid(ev_table) && ev_day_off == 0) {
        if (++ev_reload >= 20) {
            ev_reload = 0;
            fill_events();
        }
    }

    /* live ADC count while a calibration session runs (once a second) */
    static int tick;
    if (!cal_active) return;
    if (!lv_obj_is_valid(lbl_calcount)) { cal_active = 0; return; }
    if (++tick < 2) return;
    tick = 0;

    int card  = (int)lv_dropdown_get_selected(dd_scard);
    uint16_t cnt;
    if (mb_service_read(g_cfg.slave_base + card, 3,
                        REG_CAL_COUNT, 1, &cnt) == 0)
        lv_label_set_text_fmt(lbl_calcount, "%d", (int16_t)cnt);
    else
        lv_label_set_text(lbl_calcount, "----");
}
