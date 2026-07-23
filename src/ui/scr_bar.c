/* scr_bar.c - bar graph view, JUMO-style vertical bars.
 * One column per channel of the group: tag on top, large live value +
 * unit, a vertical coloured bar scaled to the channel range, the range
 * end-values beside it, and alarm HIGH/LOW limit markers on the bar. */
#include "ui.h"
#include "data_model.h"
#include <stdio.h>

#define COL_W    92
#define COL_H    380
#define TRK_W    40
#define TRK_X    ((COL_W - TRK_W) / 2)
#define TRK_Y    66
#define TRK_H    246

typedef struct {
    lv_obj_t *col;
    lv_obj_t *tag;
    lv_obj_t *val;
    lv_obj_t *unit;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *hi_lbl;
    lv_obj_t *lo_lbl;
    lv_obj_t *mk_hi;    /* alarm-HIGH limit marker */
    lv_obj_t *mk_lo;    /* alarm-LOW  limit marker */
} bar_col_t;

static bar_col_t cols[CH_PER_GROUP];
static bool built;

void scr_bar_build(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(parent, 6, 0);
    lv_obj_set_style_pad_top(parent, 4, 0);

    for (int i = 0; i < CH_PER_GROUP; i++) {
        bar_col_t *c = &cols[i];

        c->col = lv_obj_create(parent);
        lv_obj_set_size(c->col, COL_W, COL_H);
        lv_obj_set_style_bg_opa(c->col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(c->col, 0, 0);
        lv_obj_set_style_pad_all(c->col, 0, 0);
        lv_obj_set_layout(c->col, LV_LAYOUT_NONE);
        lv_obj_remove_flag(c->col, LV_OBJ_FLAG_SCROLLABLE);

        /* tag */
        c->tag = lv_label_create(c->col);
        lv_obj_set_style_text_font(c->tag, &font_units_12, 0);
        lv_obj_set_style_text_color(c->tag, COL_MUTED, 0);
        lv_obj_set_style_text_align(c->tag, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(c->tag, COL_W);
        lv_obj_set_pos(c->tag, 0, 0);

        /* big value */
        c->val = lv_label_create(c->col);
        lv_obj_set_style_text_font(c->val, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(c->val, COL_TEXT, 0);
        lv_obj_set_style_text_align(c->val, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(c->val, COL_W);
        lv_obj_set_pos(c->val, 0, 18);

        /* unit */
        c->unit = lv_label_create(c->col);
        lv_obj_set_style_text_font(c->unit, &font_units_12, 0);
        lv_obj_set_style_text_color(c->unit, COL_MUTED, 0);
        lv_obj_set_style_text_align(c->unit, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(c->unit, COL_W);
        lv_obj_set_pos(c->unit, 0, 46);

        /* range top value */
        c->hi_lbl = lv_label_create(c->col);
        lv_obj_set_style_text_font(c->hi_lbl, &font_units_12, 0);
        lv_obj_set_style_text_color(c->hi_lbl, COL_MUTED, 0);
        lv_obj_set_pos(c->hi_lbl, TRK_X + TRK_W + 2, TRK_Y - 2);

        /* bar track */
        c->track = lv_obj_create(c->col);
        lv_obj_set_size(c->track, TRK_W, TRK_H);
        lv_obj_set_pos(c->track, TRK_X, TRK_Y);
        lv_obj_set_style_bg_color(c->track, COL_PANEL, 0);
        lv_obj_set_style_border_color(c->track, COL_BORDER, 0);
        lv_obj_set_style_border_width(c->track, 1, 0);
        lv_obj_set_style_radius(c->track, 4, 0);
        lv_obj_set_style_pad_all(c->track, 0, 0);
        lv_obj_remove_flag(c->track, LV_OBJ_FLAG_SCROLLABLE);

        /* fill (grows from the bottom) */
        c->fill = lv_obj_create(c->track);
        lv_obj_set_width(c->fill, LV_PCT(100));
        lv_obj_set_height(c->fill, 0);
        lv_obj_align(c->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(c->fill, COL_ACCENT, 0);
        lv_obj_set_style_border_width(c->fill, 0, 0);
        lv_obj_set_style_radius(c->fill, 3, 0);
        lv_obj_remove_flag(c->fill, LV_OBJ_FLAG_SCROLLABLE);

        /* alarm-limit markers (thin lines across the track) */
        c->mk_hi = lv_obj_create(c->track);
        lv_obj_set_size(c->mk_hi, TRK_W, 2);
        lv_obj_set_style_bg_color(c->mk_hi, COL_ALARM, 0);
        lv_obj_set_style_border_width(c->mk_hi, 0, 0);
        lv_obj_set_style_radius(c->mk_hi, 0, 0);
        lv_obj_add_flag(c->mk_hi, LV_OBJ_FLAG_HIDDEN);

        c->mk_lo = lv_obj_create(c->track);
        lv_obj_set_size(c->mk_lo, TRK_W, 2);
        lv_obj_set_style_bg_color(c->mk_lo, COL_ALARM, 0);
        lv_obj_set_style_border_width(c->mk_lo, 0, 0);
        lv_obj_set_style_radius(c->mk_lo, 0, 0);
        lv_obj_add_flag(c->mk_lo, LV_OBJ_FLAG_HIDDEN);

        /* range bottom value */
        c->lo_lbl = lv_label_create(c->col);
        lv_obj_set_style_text_font(c->lo_lbl, &font_units_12, 0);
        lv_obj_set_style_text_color(c->lo_lbl, COL_MUTED, 0);
        lv_obj_set_pos(c->lo_lbl, TRK_X + TRK_W + 2, TRK_Y + TRK_H - 12);
    }
    built = true;
    scr_bar_refresh();
}

/* y offset (from track top) of a value on the channel's scale */
static int scale_y(float v, float lo, float hi)
{
    float span = (hi - lo) != 0 ? (hi - lo) : 1.0f;
    float f = (v - lo) / span;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return TRK_H - (int)(f * TRK_H);
}

void scr_bar_refresh(void)
{
    if (!built) return;
    int base = ui_group() * CH_PER_GROUP;

    for (int i = 0; i < CH_PER_GROUP; i++) {
        bar_col_t *c = &cols[i];
        if (!lv_obj_is_valid(c->col)) { built = false; return; }

        /* snapshot under the lock; the acquisition thread writes g_ch */
        channel_t snap;
        data_lock();
        snap = g_ch[base + i];
        data_unlock();

        lv_label_set_text_fmt(c->tag, "CH%d  %s", base + i + 1, snap.tag);
        lv_label_set_text_fmt(c->hi_lbl, "%g", (double)snap.hi);
        lv_label_set_text_fmt(c->lo_lbl, "%g", (double)snap.lo);

        lv_color_t chcol = ui_ch_color(base + i);
        bool alm = (snap.status == CH_ALM_HI || snap.status == CH_ALM_LO);
        bool ok  = (snap.status == CH_OK || alm);

        /* fill height + colour */
        float span = (snap.hi - snap.lo) != 0 ? (snap.hi - snap.lo) : 1.0f;
        float pct  = (snap.value - snap.lo) / span;
        if (pct < 0) pct = 0;
        if (pct > 1) pct = 1;
        int fh = ok ? (int)(pct * TRK_H) : 0;
        lv_obj_set_height(c->fill, fh);
        lv_obj_set_style_bg_color(c->fill, alm ? COL_ALARM : chcol, 0);

        /* alarm limit markers */
        if (snap.alm_hi > snap.lo && snap.alm_hi < snap.hi) {
            lv_obj_set_y(c->mk_hi, scale_y(snap.alm_hi, snap.lo, snap.hi) - 1);
            lv_obj_remove_flag(c->mk_hi, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c->mk_hi, LV_OBJ_FLAG_HIDDEN);
        }
        if (snap.alm_lo > snap.lo && snap.alm_lo < snap.hi) {
            lv_obj_set_y(c->mk_lo, scale_y(snap.alm_lo, snap.lo, snap.hi) - 1);
            lv_obj_remove_flag(c->mk_lo, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(c->mk_lo, LV_OBJ_FLAG_HIDDEN);
        }

        /* value + unit */
        if (!ok) {
            const char *txt = "COMM";
            switch (snap.status) {
            case CH_SKIP:  txt = "SKIP";  break;
            case CH_UNDER: txt = "UNDER"; break;
            case CH_OVER:  txt = "OVER";  break;
            case CH_OPEN:  txt = "OPEN";  break;
            default:                      break;
            }
            lv_label_set_text(c->val, txt);
            lv_obj_set_style_text_font(c->val, &font_units_16, 0);
            lv_obj_set_style_text_color(c->val, COL_MUTED, 0);
            lv_label_set_text(c->unit, "");
            lv_obj_set_style_border_color(c->track, COL_BORDER, 0);
        } else {
            lv_label_set_text_fmt(c->val, "%.1f", (double)snap.value);
            lv_obj_set_style_text_font(c->val, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(c->val,
                                        alm ? COL_ALARM_TXT : chcol, 0);
            lv_label_set_text(c->unit, snap.unit);
            lv_obj_set_style_border_color(c->track,
                                          alm ? COL_ALARM : COL_BORDER, 0);
        }
    }
}
