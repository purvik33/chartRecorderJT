/* scr_bar.c - bar graph view: one horizontal bar per channel of the group */
#include "ui.h"
#include "data_model.h"
#include <stdio.h>

typedef struct {
    lv_obj_t *row;
    lv_obj_t *lbl_tag;
    lv_obj_t *lbl_lo;
    lv_obj_t *bar;
    lv_obj_t *lbl_hi;
    lv_obj_t *lbl_val;
} bar_row_t;

static bar_row_t rows[CH_PER_GROUP];
static bool built;

void scr_bar_build(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(parent, 10, 0);

    for (int i = 0; i < CH_PER_GROUP; i++) {
        bar_row_t *r = &rows[i];

        r->row = lv_obj_create(parent);
        lv_obj_set_size(r->row, LV_PCT(100), 40);
        lv_obj_set_style_bg_opa(r->row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r->row, 0, 0);
        lv_obj_set_style_pad_all(r->row, 0, 0);
        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);

        r->lbl_tag = lv_label_create(r->row);
        lv_obj_set_style_text_color(r->lbl_tag, COL_MUTED, 0);
        lv_obj_set_style_text_font(r->lbl_tag, &font_units_12, 0);
        lv_obj_align(r->lbl_tag, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_width(r->lbl_tag, 92);

        r->lbl_lo = lv_label_create(r->row);
        lv_obj_set_style_text_color(r->lbl_lo, COL_MUTED, 0);
        lv_obj_set_style_text_font(r->lbl_lo, &font_units_12, 0);
        lv_obj_set_style_text_align(r->lbl_lo, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(r->lbl_lo, 52);
        lv_obj_align(r->lbl_lo, LV_ALIGN_LEFT_MID, 96, 0);

        r->bar = lv_bar_create(r->row);
        lv_obj_set_size(r->bar, 400, 18);
        lv_obj_align(r->bar, LV_ALIGN_LEFT_MID, 156, 0);
        lv_bar_set_range(r->bar, 0, 1000);
        lv_obj_set_style_bg_color(r->bar, COL_PANEL, LV_PART_MAIN);
        lv_obj_set_style_border_color(r->bar, COL_BORDER, LV_PART_MAIN);
        lv_obj_set_style_border_width(r->bar, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(r->bar, COL_ACCENT, LV_PART_INDICATOR);

        r->lbl_hi = lv_label_create(r->row);
        lv_obj_set_style_text_color(r->lbl_hi, COL_MUTED, 0);
        lv_obj_set_style_text_font(r->lbl_hi, &font_units_12, 0);
        lv_obj_set_width(r->lbl_hi, 52);
        lv_obj_align(r->lbl_hi, LV_ALIGN_LEFT_MID, 562, 0);

        r->lbl_val = lv_label_create(r->row);
        lv_obj_set_style_text_color(r->lbl_val, COL_TEXT, 0);
        lv_obj_set_style_text_font(r->lbl_val, &font_units_14, 0);
        lv_obj_align(r->lbl_val, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    built = true;
    scr_bar_refresh();
}

void scr_bar_refresh(void)
{
    if (!built) return;
    int base = ui_group() * CH_PER_GROUP;

    for (int i = 0; i < CH_PER_GROUP; i++) {
        bar_row_t *r = &rows[i];
        if (!lv_obj_is_valid(r->row)) { built = false; return; }
        channel_t *c = &g_ch[base + i];

        lv_label_set_text_fmt(r->lbl_tag, "CH%d %s", base + i + 1, c->tag);
        lv_label_set_text_fmt(r->lbl_lo, "%g", (double)c->lo);
        lv_label_set_text_fmt(r->lbl_hi, "%g", (double)c->hi);

        float pct = (c->value - c->lo) / (c->hi - c->lo);
        if (pct < 0) pct = 0;
        if (pct > 1) pct = 1;
        lv_bar_set_value(r->bar, (int32_t)(pct * 1000.0f), LV_ANIM_OFF);

        bool alm = (c->status == CH_ALM_HI || c->status == CH_ALM_LO);
        lv_obj_set_style_bg_color(r->bar,
                                  alm ? COL_ALARM : ui_ch_color(base + i),
                                  LV_PART_INDICATOR);
        if (c->status != CH_OK && c->status != CH_ALM_HI &&
            c->status != CH_ALM_LO) {
            const char *txt = "COMM";
            int barv = 0;
            switch (c->status) {
            case CH_SKIP:  txt = "SKIP";  break;
            case CH_UNDER: txt = "UNDER"; break;
            case CH_OVER:  txt = "OVER";  barv = 1000; break;
            case CH_OPEN:  txt = "OPEN";  break;
            default:                      break;
            }
            lv_label_set_text(r->lbl_val, txt);
            lv_obj_set_style_text_color(r->lbl_val, COL_MUTED, 0);
            lv_bar_set_value(r->bar, barv, LV_ANIM_OFF);
        } else {
            lv_label_set_text_fmt(r->lbl_val, "%.1f %s",
                                  (double)c->value, c->unit);
            lv_obj_set_style_text_color(r->lbl_val,
                                        alm ? COL_ALARM_TXT : COL_TEXT, 0);
        }
    }
}
