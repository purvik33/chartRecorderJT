/* scr_alarm.c - alarms as a table with the full lifecycle per alarm:
 * channel, type, value, generated / acknowledged / resolved times.
 *  Current: live list from RAM (active + unacknowledged)
 *  History: records rebuilt from the persistent event log, per day */
#include "ui.h"
#include "data_model.h"
#include "alarm.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SHOW_MAX 12
#define HREC_MAX 60

static lv_obj_t *table;
static lv_obj_t *lbl_summary;
static lv_obj_t *btn_ack, *btn_cur, *btn_his, *btn_hp, *btn_hn, *lbl_hday;
static int  mode;          /* 0 = current, 1 = history */
static int  hday_off;
static int  hist_reload;
static bool built;

static alarm_rec_t hrecs[HREC_MAX];

static void ack_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alarm_ack_all();
    scr_alarm_refresh();
}

static void table_header(void)
{
    static const char *hdr[7] =
        { "CH", "Tag", "Type", "Value", "Generated", "Acknowledged",
          "Resolved" };
    for (int c = 0; c < 7; c++)
        lv_table_set_cell_value(table, 0, (uint32_t)c, hdr[c]);
}

static void fmt_t(time_t t, char *out, int n)
{
    if (t == 0) { snprintf(out, n, "-"); return; }
    struct tm tm = *localtime(&t);
    snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void fill_current(void)
{
    lv_label_set_text_fmt(lbl_summary, "Active: %d   Unack: %d",
                          alarm_active_count(), alarm_unacked_count());

    alarm_evt_t evt[SHOW_MAX];
    int n = alarm_get_history(evt, SHOW_MAX);

    lv_table_set_row_count(table, (uint32_t)(n + 1));
    table_header();

    static const char *type_txt[] = { "HIGH", "LOW", "COMM" };

    for (int i = 0; i < n; i++) {
        alarm_evt_t *e = &evt[i];
        int r = i + 1;
        char b[24];

        data_lock();
        lv_table_set_cell_value(table, r, 1, g_ch[e->ch].tag);
        data_unlock();

        snprintf(b, sizeof(b), "CH%d", e->ch + 1);
        lv_table_set_cell_value(table, r, 0, b);
        lv_table_set_cell_value(table, r, 2, type_txt[e->type]);

        if (e->type == ALM_COMM) snprintf(b, sizeof(b), "-");
        else snprintf(b, sizeof(b), "%.1f", (double)e->value);
        lv_table_set_cell_value(table, r, 3, b);

        fmt_t(e->t_set, b, sizeof(b));
        lv_table_set_cell_value(table, r, 4, b);
        fmt_t(e->t_ack, b, sizeof(b));
        lv_table_set_cell_value(table, r, 5, b);
        if (e->t_clear == 0) snprintf(b, sizeof(b), "ACTIVE");
        else fmt_t(e->t_clear, b, sizeof(b));
        lv_table_set_cell_value(table, r, 6, b);
    }
}

static void fill_history(void)
{
    lv_label_set_text(lbl_summary, "Alarm log");

    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t ds = mktime(&tm) - (time_t)hday_off * 86400;
    struct tm dt = *localtime(&ds);

    lv_label_set_text_fmt(lbl_hday, "%s%02d-%02d-%04d",
                          hday_off == 0 ? "Today " : "",
                          dt.tm_mday, dt.tm_mon + 1, dt.tm_year + 1900);

    int n = alarm_records_load(ds, ds + 86399, hrecs, HREC_MAX);

    lv_table_set_row_count(table, (uint32_t)(n + 1));
    table_header();

    /* newest first */
    for (int i = 0; i < n; i++) {
        alarm_rec_t *rec = &hrecs[n - 1 - i];
        int r = i + 1;
        char b[24];

        lv_table_set_cell_value(table, r, 0, rec->ch);
        lv_table_set_cell_value(table, r, 1, rec->tag);
        lv_table_set_cell_value(table, r, 2, rec->type);

        if (!strcmp(rec->type, "COMM")) snprintf(b, sizeof(b), "-");
        else snprintf(b, sizeof(b), "%.1f", (double)rec->value);
        lv_table_set_cell_value(table, r, 3, b);

        lv_table_set_cell_value(table, r, 4,
            strlen(rec->set_ts) > 11 ? rec->set_ts + 11 : rec->set_ts);
        lv_table_set_cell_value(table, r, 5,
            rec->ack_ts[0] ? (strlen(rec->ack_ts) > 11 ?
                              rec->ack_ts + 11 : rec->ack_ts) : "-");
        lv_table_set_cell_value(table, r, 6,
            rec->clr_ts[0] ? (strlen(rec->clr_ts) > 11 ?
                              rec->clr_ts + 11 : rec->clr_ts) : "ACTIVE");
    }
}

static void mode_style(void)
{
    lv_obj_set_style_bg_color(btn_cur, mode == 0 ? COL_ACCENT : COL_PANEL, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_cur, 0),
                                mode == 0 ? COL_BG : COL_MUTED, 0);
    lv_obj_set_style_bg_color(btn_his, mode == 1 ? COL_ACCENT : COL_PANEL, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(btn_his, 0),
                                mode == 1 ? COL_BG : COL_MUTED, 0);

    if (mode == 1) {
        lv_obj_remove_flag(btn_hp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_hn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(lbl_hday, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_ack, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn_hp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(btn_hn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_hday, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(btn_ack, LV_OBJ_FLAG_HIDDEN);
    }
}

static void mode_cb(lv_event_t *e)
{
    mode = (int)(intptr_t)lv_event_get_user_data(e);
    mode_style();
    if (mode == 1) fill_history();
    else           fill_current();
}

static void hprev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hday_off++;
    fill_history();
}

static void hnext_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (hday_off > 0) { hday_off--; fill_history(); }
}

static lv_obj_t *small_btn(lv_obj_t *parent, const char *txt,
                           lv_event_cb_t cb, void *ud, int w)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 34);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &font_units_14, 0);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);
    return b;
}

void scr_alarm_build(lv_obj_t *parent)
{
    lv_obj_t *top = lv_obj_create(parent);
    lv_obj_set_size(top, LV_PCT(100), 44);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_hor(top, 12, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    btn_cur = small_btn(top, "Current", mode_cb, (void *)(intptr_t)0, 96);
    lv_obj_align(btn_cur, LV_ALIGN_LEFT_MID, 0, 0);
    btn_his = small_btn(top, "History", mode_cb, (void *)(intptr_t)1, 96);
    lv_obj_align(btn_his, LV_ALIGN_LEFT_MID, 102, 0);

    btn_hp = small_btn(top, LV_SYMBOL_LEFT, hprev_cb, NULL, 40);
    lv_obj_align(btn_hp, LV_ALIGN_LEFT_MID, 220, 0);
    lbl_hday = lv_label_create(top);
    lv_obj_set_style_text_font(lbl_hday, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_hday, COL_TEXT, 0);
    lv_obj_align(lbl_hday, LV_ALIGN_LEFT_MID, 268, 0);
    lv_obj_set_width(lbl_hday, 146);
    btn_hn = small_btn(top, LV_SYMBOL_RIGHT, hnext_cb, NULL, 40);
    lv_obj_align(btn_hn, LV_ALIGN_LEFT_MID, 418, 0);

    lbl_summary = lv_label_create(top);
    lv_obj_set_style_text_color(lbl_summary, COL_MUTED, 0);
    lv_obj_set_style_text_font(lbl_summary, &font_units_14, 0);
    lv_obj_align(lbl_summary, LV_ALIGN_RIGHT_MID, -190, 0);

    btn_ack = lv_button_create(top);
    lv_obj_set_size(btn_ack, 175, 34);
    lv_obj_align(btn_ack, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_ack, COL_PANEL, 0);
    lv_obj_set_style_border_color(btn_ack, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn_ack, 1, 0);
    lv_obj_set_style_shadow_width(btn_ack, 0, 0);
    lv_obj_add_event_cb(btn_ack, ack_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn_ack);
    lv_label_set_text(bl, LV_SYMBOL_OK "  Acknowledge all");
    lv_obj_set_style_text_font(bl, &font_units_14, 0);
    lv_obj_set_style_text_color(bl, COL_TEXT, 0);
    lv_obj_center(bl);

    /* ---- alarm table ---- */
    table = lv_table_create(parent);
    lv_obj_set_size(table, LV_PCT(98), 480 - 40 - 56 - 50);
    lv_obj_align(table, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(table, COL_PANEL, 0);
    lv_obj_set_style_border_color(table, COL_BORDER, 0);
    lv_obj_set_style_bg_color(table, COL_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(table, COL_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(table, &font_units_14, LV_PART_ITEMS);
    lv_obj_set_style_border_color(table, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table, 8, LV_PART_ITEMS);

    lv_table_set_column_count(table, 7);
    lv_table_set_column_width(table, 0, 64);
    lv_table_set_column_width(table, 1, 112);
    lv_table_set_column_width(table, 2, 86);
    lv_table_set_column_width(table, 3, 76);
    lv_table_set_column_width(table, 4, 140);
    lv_table_set_column_width(table, 5, 140);
    lv_table_set_column_width(table, 6, 140);

    built = true;
    hist_reload = 0;
    mode_style();
    if (mode == 1) fill_history();
    else           fill_current();
}

void scr_alarm_refresh(void)
{
    if (!built || !lv_obj_is_valid(table)) { built = false; return; }

    if (mode == 0) {
        fill_current();
    } else if (hday_off == 0 && ++hist_reload >= 20) {
        hist_reload = 0;
        fill_history();
    }
}
