/* scr_alarm.c - alarms as a table with the full lifecycle per alarm:
 * channel, type, value, generated / acknowledged / resolved times.
 *  Current: live list from RAM (active + unacknowledged)
 *  History: records rebuilt from the persistent event log, per day */
#include "ui.h"
#include "data_model.h"
#include "alarm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE_SZ  10        /* rows shown per page (same as the web dashboard) */
#define CUR_MAX  ALARM_HIST
#define HREC_MAX 100

static lv_obj_t *table;
static lv_obj_t *lbl_summary;
static lv_obj_t *btn_ack, *btn_cur, *btn_his, *btn_hp, *btn_hn, *lbl_hday;
static lv_obj_t *btn_pp, *btn_pn, *lbl_page;   /* bottom pager */
static int  mode;          /* 0 = current, 1 = history */
static int  hday_off;
static int  hist_reload;
static bool built;

/* loaded data sets + current page. Both lists are rendered newest-first and
 * only one page of PAGE_SZ rows is drawn at a time, so a long alarm log no
 * longer paints hundreds of table rows in one go. */
static alarm_evt_t cur_evt[CUR_MAX];
static int         cur_n;
static alarm_rec_t hrecs[HREC_MAX];
static int         hist_n;
static int         page;

static void render_page(void);

static void ack_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    alarm_ack_all();
    page = 0;
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

/* draw one page (PAGE_SZ rows) of the active list, newest-first, and update
 * the pager controls. Called after a load, and by the page prev/next. */
static void render_page(void)
{
    int total = (mode == 0) ? cur_n : hist_n;
    int pages = total ? (total + PAGE_SZ - 1) / PAGE_SZ : 1;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;

    int start = page * PAGE_SZ;
    int cnt = total - start;
    if (cnt > PAGE_SZ) cnt = PAGE_SZ;
    if (cnt < 0) cnt = 0;

    lv_table_set_row_count(table, (uint32_t)(cnt + 1));
    table_header();

    static const char *type_txt[] = { "HIGH", "LOW", "COMM", "OPEN" };
    char b[24];

    for (int i = 0; i < cnt; i++) {
        int r = i + 1;
        int g = start + i;               /* newest-first index into the set */

        if (mode == 0) {
            alarm_evt_t *e = &cur_evt[g];
            data_lock();
            lv_table_set_cell_value(table, r, 1, g_ch[e->ch].tag);
            data_unlock();
            snprintf(b, sizeof(b), "CH%d", e->ch + 1);
            lv_table_set_cell_value(table, r, 0, b);
            lv_table_set_cell_value(table, r, 2, type_txt[e->type]);
            if (e->type == ALM_COMM) snprintf(b, sizeof(b), "-");
            else disp_str(b, sizeof(b), (double)e->value,
                          e->ch >= 0 && e->ch < CH_TOTAL ? ch_dec(&g_ch[e->ch]) : 1);
            lv_table_set_cell_value(table, r, 3, b);
            fmt_t(e->t_set, b, sizeof(b));
            lv_table_set_cell_value(table, r, 4, b);
            fmt_t(e->t_ack, b, sizeof(b));
            lv_table_set_cell_value(table, r, 5, b);
            if (e->t_clear == 0) snprintf(b, sizeof(b), "ACTIVE");
            else fmt_t(e->t_clear, b, sizeof(b));
            lv_table_set_cell_value(table, r, 6, b);
        } else {
            alarm_rec_t *rec = &hrecs[hist_n - 1 - g];   /* chrono -> newest */
            lv_table_set_cell_value(table, r, 0, rec->ch);
            lv_table_set_cell_value(table, r, 1, rec->tag);
            lv_table_set_cell_value(table, r, 2, rec->type);
            if (!strcmp(rec->type, "COMM")) snprintf(b, sizeof(b), "-");
            else {
                int ri = (rec->ch[0] == 'C' && rec->ch[1] == 'H')
                         ? atoi(rec->ch + 2) - 1 : -1;
                disp_str(b, sizeof(b), (double)rec->value,
                         ri >= 0 && ri < CH_TOTAL ? ch_dec(&g_ch[ri]) : 1);
            }
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

    /* pager controls */
    if (total == 0) lv_label_set_text(lbl_page, "No alarms");
    else lv_label_set_text_fmt(lbl_page, "Page %d / %d   (%d)",
                               page + 1, pages, total);
    if (page <= 0)        lv_obj_add_state(btn_pp, LV_STATE_DISABLED);
    else                  lv_obj_remove_state(btn_pp, LV_STATE_DISABLED);
    if (page >= pages - 1) lv_obj_add_state(btn_pn, LV_STATE_DISABLED);
    else                   lv_obj_remove_state(btn_pn, LV_STATE_DISABLED);
}

static void fill_current(void)
{
    lv_label_set_text_fmt(lbl_summary, "Active: %d   Unack: %d",
                          alarm_active_count(), alarm_unacked_count());
    cur_n = alarm_get_history(cur_evt, CUR_MAX);   /* newest first */
    render_page();
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

    hist_n = alarm_records_load(ds, ds + 86399, hrecs, HREC_MAX);
    render_page();
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
    page = 0;
    mode_style();
    if (mode == 1) fill_history();
    else           fill_current();
}

static void hprev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    hday_off++;
    page = 0;
    fill_history();
}

static void hnext_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (hday_off > 0) { hday_off--; page = 0; fill_history(); }
}

static void ppage_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (page > 0) { page--; render_page(); }
}

static void npage_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    page++;                 /* render_page() clamps to the last page */
    render_page();
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

    /* ---- alarm table ---- (leaves 40 px at the bottom for the pager) */
    table = lv_table_create(parent);
    lv_obj_set_size(table, LV_PCT(98), 480 - 40 - 56 - 50 - 40);
    lv_obj_align(table, LV_ALIGN_BOTTOM_MID, 0, -42);
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

    /* ---- bottom pager bar: prev | Page x / y | next ---- */
    lv_obj_t *pager_bar = lv_obj_create(parent);
    lv_obj_set_size(pager_bar, LV_PCT(98), 36);
    lv_obj_align(pager_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(pager_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pager_bar, 0, 0);
    lv_obj_set_style_pad_all(pager_bar, 0, 0);
    lv_obj_set_style_pad_column(pager_bar, 16, 0);
    lv_obj_remove_flag(pager_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(pager_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager_bar, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btn_pp = small_btn(pager_bar, LV_SYMBOL_LEFT, ppage_cb, NULL, 64);
    lbl_page = lv_label_create(pager_bar);
    lv_label_set_text(lbl_page, "Page 1 / 1");
    lv_obj_set_style_text_font(lbl_page, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_page, COL_TEXT, 0);
    lv_obj_set_width(lbl_page, 180);
    lv_obj_set_style_text_align(lbl_page, LV_TEXT_ALIGN_CENTER, 0);
    btn_pn = small_btn(pager_bar, LV_SYMBOL_RIGHT, npage_cb, NULL, 64);

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
