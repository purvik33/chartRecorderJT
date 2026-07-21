/* scr_digital.c - home screen: 4x2 grid of channel value tiles.
 * Tapping a tile opens a popup with that channel's live mini-trend
 * (last 10 minutes from the 1 s RAM ring, refreshed every second). */
#include "ui.h"
#include "data_model.h"
#include <stdio.h>
#include <time.h>

typedef struct {
    lv_obj_t *tile;
    lv_obj_t *lbl_tag;
    lv_obj_t *lbl_sensor;
    lv_obj_t *lbl_value;
    lv_obj_t *lbl_unit;
    lv_obj_t *lbl_sp_l;    /* alarm setpoint L: bottom left  */
    lv_obj_t *lbl_sp_h;    /* alarm setpoint H: bottom right */
} tile_t;

static tile_t tiles[CH_PER_GROUP];
static bool built;

/* ---- channel mini-trend popup ------------------------------------------ */

#define POP_PTS  300           /* 10 minutes, one point per 2 s */
#define POP_SPAN 600

static lv_obj_t *pop;          /* dimmed overlay (NULL = closed) */
static lv_obj_t *pop_title, *pop_val, *pop_stats, *pop_ylo, *pop_yhi;
static lv_obj_t *pop_chart;
static lv_chart_series_t *pop_ser;
static lv_timer_t *pop_timer;
static int pop_ch = -1;        /* absolute channel index */

static void pop_fill(void)
{
    channel_t snap;
    data_lock();
    snap = g_ch[pop_ch];
    data_unlock();

    lv_label_set_text_fmt(pop_title, "CH%d  %s", pop_ch + 1, snap.tag);

    if (snap.status == CH_OK || snap.status == CH_ALM_HI ||
        snap.status == CH_ALM_LO)
        lv_label_set_text_fmt(pop_val, "%.1f %s", (double)snap.value,
                              snap.unit);
    else
        lv_label_set_text(pop_val,
            snap.status == CH_SKIP  ? "SKIP"  :
            snap.status == CH_UNDER ? "UNDER" :
            snap.status == CH_OVER  ? "OVER"  :
            snap.status == CH_OPEN  ? "OPEN"  : "COMM");

    /* last 10 minutes from the live ring, 2 s per chart point */
    static float eng[POP_PTS];
    static uint8_t have[POP_PTS];
    time_t now = time(NULL);
    float vmin = 0, vmax = 0, vsum = 0;
    int vany = 0, vcnt = 0;

    for (int p = 0; p < POP_PTS; p++) {
        float sum = 0;
        int n = 0;
        for (int k = 0; k < 2; k++) {
            time_t ts = now - POP_SPAN + 1 + p * 2 + k;
            float v;
            if (data_live_get(pop_ch, ts, &v)) { sum += v; n++; }
        }
        have[p] = (uint8_t)(n > 0);
        if (n > 0) {
            eng[p] = sum / (float)n;
            if (!vany) { vmin = vmax = eng[p]; vany = 1; }
            if (eng[p] < vmin) vmin = eng[p];
            if (eng[p] > vmax) vmax = eng[p];
            vsum += eng[p];
            vcnt++;
        }
    }

    if (!vany) { vmin = 0; vmax = 100; }
    float sp = vmax - vmin;
    if (sp < 1e-6f) { vmin -= 1.0f; vmax += 1.0f; sp = 2.0f; }
    float lo = vmin - sp * 0.10f, hi = vmax + sp * 0.10f;

    lv_label_set_text_fmt(pop_yhi, "%g", (double)hi);
    lv_label_set_text_fmt(pop_ylo, "%g", (double)lo);

    for (int p = 0; p < POP_PTS; p++)
        lv_chart_set_value_by_id(pop_chart, pop_ser, (uint32_t)p,
            have[p] ? (int32_t)((eng[p] - lo) / (hi - lo) * 1000.0f)
                    : LV_CHART_POINT_NONE);
    lv_chart_refresh(pop_chart);

    if (vcnt > 0)
        lv_label_set_text_fmt(pop_stats,
            "Last 10 min      Min %.1f     Max %.1f     Avg %.1f  %s",
            (double)vmin, (double)vmax, (double)(vsum / (float)vcnt),
            snap.unit);
    else
        lv_label_set_text(pop_stats, "Last 10 min - no data yet");
}

static void pop_tick(lv_timer_t *t)
{
    if (!pop || !lv_obj_is_valid(pop)) {
        lv_timer_delete(t);
        pop_timer = NULL;
        pop = NULL;
        return;
    }
    /* auto-close after 30 s without any touch, so a forgotten popup
     * returns the panel to the overview on its own */
    if (lv_display_get_inactive_time(NULL) > 30000) {
        lv_timer_delete(t);
        pop_timer = NULL;
        lv_obj_delete(pop);
        pop = NULL;
        pop_ch = -1;
        return;
    }
    pop_fill();
}

static void pop_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (pop) {
        lv_obj_delete(pop);
        pop = NULL;
        pop_ch = -1;
    }
}

static void pop_open(lv_obj_t *parent, int ch)
{
    pop_ch = ch;

    pop = lv_obj_create(parent);
    lv_obj_set_size(pop, LV_PCT(100), LV_PCT(100));
    /* overlay, not a grid cell: opt out of the tiles' flex layout */
    lv_obj_add_flag(pop, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(pop, 0, 0);
    lv_obj_set_style_bg_color(pop, COL_BG, 0);
    lv_obj_set_style_bg_opa(pop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(pop, 0, 0);
    lv_obj_set_style_radius(pop, 0, 0);
    lv_obj_set_style_pad_all(pop, 0, 0);
    lv_obj_remove_flag(pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pop, pop_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(pop);
    lv_obj_set_size(box, 660, 370);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COL_PANEL, 0);
    lv_obj_set_style_border_color(box, ui_ch_color(ch), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    pop_title = lv_label_create(box);
    lv_obj_set_style_text_font(pop_title, &font_units_16, 0);
    lv_obj_set_style_text_color(pop_title, ui_ch_color(ch), 0);
    lv_obj_align(pop_title, LV_ALIGN_TOP_LEFT, 2, 0);

    pop_val = lv_label_create(box);
    lv_obj_set_style_text_font(pop_val, &font_units_28, 0);
    lv_obj_set_style_text_color(pop_val, COL_TEXT, 0);
    lv_obj_align(pop_val, LV_ALIGN_TOP_MID, 30, -4);

    lv_obj_t *close = lv_button_create(box);
    lv_obj_set_size(close, 44, 38);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close, COL_BG, 0);
    lv_obj_set_style_border_color(close, COL_BORDER, 0);
    lv_obj_set_style_border_width(close, 1, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, pop_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(cl, COL_TEXT, 0);
    lv_obj_center(cl);

    pop_chart = lv_chart_create(box);
    lv_obj_set_size(pop_chart, 574, 236);
    lv_obj_align(pop_chart, LV_ALIGN_TOP_MID, 22, 44);
    lv_chart_set_type(pop_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(pop_chart, POP_PTS);
    lv_chart_set_range(pop_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_div_line_count(pop_chart, 5, 7);
    lv_obj_set_style_bg_color(pop_chart, COL_BG, 0);
    lv_obj_set_style_border_color(pop_chart, COL_BORDER, 0);
    lv_obj_set_style_border_width(pop_chart, 1, 0);
    lv_obj_set_style_line_color(pop_chart, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_line_opa(pop_chart, 120, LV_PART_MAIN);
    lv_obj_set_style_line_width(pop_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(pop_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(pop_chart, LV_OBJ_FLAG_CLICKABLE);

    pop_ser = lv_chart_add_series(pop_chart, ui_ch_color(ch),
                                  LV_CHART_AXIS_PRIMARY_Y);

    pop_yhi = lv_label_create(box);
    lv_obj_set_style_text_font(pop_yhi, &font_units_12, 0);
    lv_obj_set_style_text_color(pop_yhi, COL_MUTED, 0);
    lv_obj_align(pop_yhi, LV_ALIGN_TOP_LEFT, 0, 44);

    pop_ylo = lv_label_create(box);
    lv_obj_set_style_text_font(pop_ylo, &font_units_12, 0);
    lv_obj_set_style_text_color(pop_ylo, COL_MUTED, 0);
    lv_obj_align(pop_ylo, LV_ALIGN_LEFT_MID, 0, 118);

    pop_stats = lv_label_create(box);
    lv_obj_set_style_text_font(pop_stats, &font_units_14, 0);
    lv_obj_set_style_text_color(pop_stats, COL_MUTED, 0);
    lv_obj_align(pop_stats, LV_ALIGN_BOTTOM_MID, 0, 0);

    pop_fill();
    pop_timer = lv_timer_create(pop_tick, 1000, NULL);
}

static void tile_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (pop) return;                    /* one popup at a time */
    pop_open(lv_obj_get_parent(tiles[i].tile),
             ui_group() * CH_PER_GROUP + i);
}

void scr_digital_build(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_row(parent, 8, 0);
    lv_obj_set_style_pad_column(parent, 8, 0);

    for (int i = 0; i < CH_PER_GROUP; i++) {
        tile_t *t = &tiles[i];

        t->tile = lv_obj_create(parent);
        lv_obj_set_size(t->tile, 188, 178);
        lv_obj_set_style_bg_color(t->tile, COL_PANEL, 0);
        lv_obj_set_style_border_color(t->tile, COL_BORDER, 0);
        lv_obj_set_style_border_width(t->tile, 1, 0);
        lv_obj_set_style_radius(t->tile, 6, 0);
        lv_obj_set_style_pad_all(t->tile, 10, 0);
        lv_obj_remove_flag(t->tile, LV_OBJ_FLAG_SCROLLABLE);
        /* tap a tile: popup with this channel's live mini-trend */
        lv_obj_add_flag(t->tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t->tile, tile_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        t->lbl_tag = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_tag, COL_MUTED, 0);
        lv_obj_set_style_text_font(t->lbl_tag, &font_units_12, 0);
        lv_obj_align(t->lbl_tag, LV_ALIGN_TOP_LEFT, 0, 0);

        t->lbl_sensor = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_sensor, COL_MUTED, 0);
        lv_obj_set_style_text_font(t->lbl_sensor, &font_units_12, 0);
        lv_obj_align(t->lbl_sensor, LV_ALIGN_TOP_RIGHT, 0, 0);

        t->lbl_value = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_value, COL_TEXT, 0);
        lv_obj_set_style_text_font(t->lbl_value, &lv_font_montserrat_40, 0);
        lv_obj_align(t->lbl_value, LV_ALIGN_CENTER, 0, -2);

        /* unit sits beside the value (aligned in refresh) */
        t->lbl_unit = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_unit, COL_MUTED, 0);
        lv_obj_set_style_text_font(t->lbl_unit, &font_units_14, 0);

        /* alarm setpoints in the bottom corners */
        t->lbl_sp_l = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_sp_l, COL_MUTED, 0);
        lv_obj_set_style_text_font(t->lbl_sp_l, &font_units_12, 0);
        lv_obj_align(t->lbl_sp_l, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        t->lbl_sp_h = lv_label_create(t->tile);
        lv_obj_set_style_text_color(t->lbl_sp_h, COL_MUTED, 0);
        lv_obj_set_style_text_font(t->lbl_sp_h, &font_units_12, 0);
        lv_obj_align(t->lbl_sp_h, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
    built = true;
    scr_digital_refresh();
}

void scr_digital_refresh(void)
{
    if (!built) return;
    int base = ui_group() * CH_PER_GROUP;

    for (int i = 0; i < CH_PER_GROUP; i++) {
        tile_t *t = &tiles[i];
        if (!lv_obj_is_valid(t->tile)) { built = false; return; }
        channel_t *c = &g_ch[base + i];

        lv_label_set_text_fmt(t->lbl_tag, "CH%d  %s", base + i + 1, c->tag);
        lv_label_set_text_fmt(t->lbl_sp_l, "L: %g", (double)c->alm_lo);
        lv_label_set_text_fmt(t->lbl_sp_h, "H: %g", (double)c->alm_hi);

        if (c->status != CH_OK && c->status != CH_ALM_HI &&
            c->status != CH_ALM_LO) {
            const char *vtxt = "----", *stxt = "COMM";
            lv_color_t vcol = COL_MUTED, scol = COL_ALARM_TXT;
            switch (c->status) {
            case CH_SKIP:  vtxt = "----";  stxt = "SKIP";
                           scol = COL_MUTED;                       break;
            case CH_UNDER: vtxt = "UNDER"; stxt = "RANGE";
                           vcol = COL_ALARM_TXT;                   break;
            case CH_OVER:  vtxt = "OVER";  stxt = "RANGE";
                           vcol = COL_ALARM_TXT;                   break;
            case CH_OPEN:  vtxt = "OPEN";  stxt = "BURNOUT";
                           vcol = COL_ALARM_TXT;                   break;
            default:                                              break;
            }
            lv_label_set_text(t->lbl_value, vtxt);
            lv_obj_set_style_bg_color(t->tile, COL_PANEL, 0);
            lv_obj_set_style_border_color(t->tile, COL_BORDER, 0);
            lv_obj_set_style_text_color(t->lbl_value, vcol, 0);
            lv_label_set_text(t->lbl_sensor, stxt);
            lv_obj_set_style_text_color(t->lbl_sensor, scol, 0);
            /* no unit while the value shows a status word */
            lv_label_set_text(t->lbl_unit, "");
            continue;
        }
        lv_label_set_text(t->lbl_unit, c->unit);
        lv_label_set_text_fmt(t->lbl_value, "%.1f", (double)c->value);

        if (c->status == CH_ALM_HI || c->status == CH_ALM_LO) {
            lv_obj_set_style_bg_color(t->tile, COL_ALARM_BG, 0);
            lv_obj_set_style_border_color(t->tile, COL_ALARM, 0);
            lv_obj_set_style_text_color(t->lbl_value, COL_ALARM_TXT, 0);
            lv_label_set_text(t->lbl_sensor,
                              c->status == CH_ALM_HI
                              ? LV_SYMBOL_BELL "  HI"
                              : LV_SYMBOL_BELL "  LO");
            lv_obj_set_style_text_color(t->lbl_sensor, COL_ALARM_TXT, 0);
        } else {
            /* channel colour: tinted tile background, coloured value */
            lv_color_t cc = ui_ch_color(base + i);
            lv_obj_set_style_bg_color(t->tile,
                lv_color_mix(cc, COL_PANEL, 40), 0);
            lv_obj_set_style_border_color(t->tile,
                lv_color_mix(cc, COL_BORDER, 90), 0);
            lv_obj_set_style_text_color(t->lbl_value, cc, 0);
            lv_label_set_text(t->lbl_sensor, c->sensor);
            lv_obj_set_style_text_color(t->lbl_sensor, COL_MUTED, 0);
        }
        /* unit below the value, right-aligned to it */
        lv_obj_align_to(t->lbl_unit, t->lbl_value,
                        LV_ALIGN_OUT_BOTTOM_RIGHT, 0, 2);
    }
}
