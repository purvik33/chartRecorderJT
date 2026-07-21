/* scr_polar.c - circular chart view (classic chart-recorder style).
 * Time runs clockwise around the dial (24 h, midnight at top),
 * value is the radius. All 8 channels of the group, group's common
 * engineering range. Today's data, refreshed automatically. */
#include "ui.h"
#include "data_model.h"
#include "history.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define CAN_SIZE 360
#define CX       (CAN_SIZE / 2)
#define CY       (CAN_SIZE / 2)
#define R_IN     34.0f
#define R_OUT    168.0f
#define P_DRAW   720          /* drawn points around the circle */

static const uint32_t series_colors[CH_PER_GROUP] = {
    0x5DCAA5, 0x85B7EB, 0xF0997B, 0xED93B1,
    0xFAC775, 0xAFA9EC, 0x97C459, 0xF09595
};

static trend_day_t pday;
static float chlo[CH_PER_GROUP], chhi[CH_PER_GROUP];
static int   reload_cnt;
static int   p_day_off;      /* 0 = today, 1 = yesterday, ... */

static lv_obj_t *canvas, *lbl_date, *lbl_hub_t, *lbl_hub_d;
static lv_obj_t *btn_ptick[CH_PER_GROUP];
static lv_obj_t *lbl_ptag[CH_PER_GROUP], *lbl_pavg[CH_PER_GROUP],
                *lbl_pmm[CH_PER_GROUP];
static uint8_t cbuf[CAN_SIZE * CAN_SIZE * 4];
static bool built;

static time_t today_start(void)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static void polar_point(int bucket, float frac, float *x, float *y)
{
    float ang = ((float)bucket / (float)P_DRAW) * 2.0f * 3.1415926f
                - 3.1415926f / 2.0f;              /* midnight at top */
    float r = R_IN + frac * (R_OUT - R_IN);
    *x = (float)CX + r * cosf(ang);
    *y = (float)CY + r * sinf(ang);
}

static void draw_chart(void)
{
    lv_canvas_fill_bg(canvas, COL_BG, LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /* dial background disc */
    lv_draw_arc_dsc_t bg;
    lv_draw_arc_dsc_init(&bg);
    bg.center.x = CX; bg.center.y = CY;
    bg.radius = (int16_t)R_OUT + 4;
    bg.width  = (int16_t)R_OUT + 4;
    bg.start_angle = 0; bg.end_angle = 360;
    bg.color = COL_PANEL;
    lv_draw_arc(&layer, &bg);

    /* concentric grid rings at 0/25/50/75/100 % */
    for (int i = 0; i <= 4; i++) {
        lv_draw_arc_dsc_t g;
        lv_draw_arc_dsc_init(&g);
        g.center.x = CX; g.center.y = CY;
        g.radius = (int16_t)(R_IN + (R_OUT - R_IN) * (float)i / 4.0f);
        g.width  = 1;
        g.start_angle = 0; g.end_angle = 360;
        g.color = COL_BORDER;
        lv_draw_arc(&layer, &g);
    }

    /* radial spokes every 2 hours */
    for (int s = 0; s < 12; s++) {
        float ang = (float)s / 12.0f * 2.0f * 3.1415926f - 3.1415926f / 2.0f;
        lv_draw_line_dsc_t sp;
        lv_draw_line_dsc_init(&sp);
        sp.p1.x = (float)CX + R_IN * cosf(ang);
        sp.p1.y = (float)CY + R_IN * sinf(ang);
        sp.p2.x = (float)CX + R_OUT * cosf(ang);
        sp.p2.y = (float)CY + R_OUT * sinf(ang);
        sp.color = COL_BORDER;
        sp.width = 1;
        lv_draw_line(&layer, &sp);
    }

    /* channel traces - each channel scaled to its OWN range (0-100 %) */
    int step = TREND_BUCKETS / P_DRAW;

    for (int c = 0; c < CH_PER_GROUP; c++) {
        if (!ui_ch_visible(c)) continue;
        float span = (chhi[c] - chlo[c]) != 0 ? (chhi[c] - chlo[c]) : 1.0f;

        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.color = lv_color_hex(series_colors[c]);
        ln.width = 2;
        ln.round_start = 1;
        ln.round_end   = 1;

        int   have_prev = 0;
        float px = 0, py = 0;

        for (int p = 0; p < P_DRAW; p++) {
            float sum = 0;
            int   n   = 0;
            for (int b = p * step; b < (p + 1) * step; b++) {
                if (pday.ok[c][b]) { sum += pday.val[c][b]; n++; }
            }
            if (n == 0) { have_prev = 0; continue; }

            float frac = ((sum / (float)n) - chlo[c]) / span;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;

            float x, y;
            polar_point(p, frac, &x, &y);
            if (have_prev) {
                ln.p1.x = px; ln.p1.y = py;
                ln.p2.x = x;  ln.p2.y = y;
                lv_draw_line(&layer, &ln);
            }
            px = x; py = y;
            have_prev = 1;
        }
    }

    /* current time marker (the "pen" position) - today only */
    if (p_day_off == 0) {
        time_t now = time(NULL);
        struct tm tm = *localtime(&now);
        int sod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        float ang = ((float)sod / 86400.0f) * 2.0f * 3.1415926f
                    - 3.1415926f / 2.0f;
        lv_draw_line_dsc_t mk;
        lv_draw_line_dsc_init(&mk);
        mk.p1.x = (float)CX + (R_IN - 6.0f) * cosf(ang);
        mk.p1.y = (float)CY + (R_IN - 6.0f) * sinf(ang);
        mk.p2.x = (float)CX + (R_OUT + 4.0f) * cosf(ang);
        mk.p2.y = (float)CY + (R_OUT + 4.0f) * sinf(ang);
        mk.color = COL_ACCENT;
        mk.width = 2;
        lv_draw_line(&layer, &mk);
    }

    /* center hub for the date/time labels */
    lv_draw_arc_dsc_t hub;
    lv_draw_arc_dsc_init(&hub);
    hub.center.x = CX; hub.center.y = CY;
    hub.radius = (int16_t)R_IN - 4;
    hub.width  = (int16_t)R_IN - 4;
    hub.start_angle = 0; hub.end_angle = 360;
    hub.color = COL_BG;
    lv_draw_arc(&layer, &hub);

    lv_draw_arc_dsc_t hubr;
    lv_draw_arc_dsc_init(&hubr);
    hubr.center.x = CX; hubr.center.y = CY;
    hubr.radius = (int16_t)R_IN - 4;
    hubr.width  = 1;
    hubr.start_angle = 0; hubr.end_angle = 360;
    hubr.color = COL_BORDER;
    lv_draw_arc(&layer, &hubr);

    lv_canvas_finish_layer(canvas, &layer);
}

static void load_day(void)
{
    int base = ui_group() * CH_PER_GROUP;
    time_t ds = today_start() - (time_t)p_day_off * 86400;
    history_load(ds, ui_group(), &pday);

    data_lock();
    for (int c = 0; c < CH_PER_GROUP; c++) {
        chlo[c] = g_ch[base + c].lo;
        chhi[c] = g_ch[base + c].hi;
    }
    data_unlock();

    struct tm dt = *localtime(&ds);
    if (p_day_off == 0) {
        time_t now = time(NULL);
        struct tm tm = *localtime(&now);
        lv_label_set_text_fmt(lbl_date, "Today %02d-%02d-%04d",
                              tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        lv_label_set_text_fmt(lbl_hub_t, "%02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
        lv_label_set_text_fmt(lbl_date, "%02d-%02d-%04d",
                              dt.tm_mday, dt.tm_mon + 1, dt.tm_year + 1900);
        lv_label_set_text(lbl_hub_t, "history");
    }
    lv_label_set_text_fmt(lbl_hub_d, "%02d-%02d", dt.tm_mday, dt.tm_mon + 1);
    lv_obj_align_to(lbl_hub_t, canvas, LV_ALIGN_CENTER, 0, -8);
    lv_obj_align_to(lbl_hub_d, canvas, LV_ALIGN_CENTER, 0, 10);

    /* per-channel 24 h statistics of the displayed day */
    data_lock();
    for (int c = 0; c < CH_PER_GROUP; c++) {
        float mn = 0, mx = 0, sum = 0;
        int n = 0;
        for (int b = 0; b < TREND_BUCKETS; b++) {
            if (!pday.ok[c][b]) continue;
            float v = pday.val[c][b];
            if (n == 0) { mn = mx = v; }
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            sum += v;
            n++;
        }
        if (n > 0) {
            lv_label_set_text_fmt(lbl_pavg[c], "%.1f %s",
                (double)(sum / (float)n), g_ch[base + c].unit);
            lv_label_set_text_fmt(lbl_pmm[c], "Min %.1f  Max %.1f",
                (double)mn, (double)mx);
        } else {
            lv_label_set_text(lbl_pavg[c], "no data");
            lv_label_set_text(lbl_pmm[c], "");
        }
    }
    data_unlock();
    draw_chart();
}

static void ptick_style_update(void)
{
    for (int i = 0; i < CH_PER_GROUP; i++) {
        if (!btn_ptick[i]) continue;
        int on = ui_ch_visible(i);
        lv_color_t cc = lv_color_hex(series_colors[i]);
        if (on) {
            lv_obj_set_style_bg_color(btn_ptick[i],
                lv_color_mix(cc, COL_PANEL, 40), 0);
            lv_obj_set_style_border_color(btn_ptick[i], cc, 0);
            lv_obj_set_style_border_width(btn_ptick[i], 2, 0);
            lv_obj_set_style_text_color(lbl_ptag[i], cc, 0);
            lv_obj_set_style_text_color(lbl_pavg[i], COL_TEXT, 0);
            lv_obj_set_style_text_color(lbl_pmm[i], COL_MUTED, 0);
        } else {
            lv_obj_set_style_bg_color(btn_ptick[i], COL_PANEL, 0);
            lv_obj_set_style_border_color(btn_ptick[i], COL_BORDER, 0);
            lv_obj_set_style_border_width(btn_ptick[i], 1, 0);
            lv_obj_set_style_text_color(lbl_ptag[i], COL_MUTED, 0);
            lv_obj_set_style_text_color(lbl_pavg[i], COL_MUTED, 0);
            lv_obj_set_style_text_color(lbl_pmm[i], COL_MUTED, 0);
        }
    }
}

static void ptick_cb(lv_event_t *e)
{
    ui_ch_visible_toggle((int)(intptr_t)lv_event_get_user_data(e));
    ptick_style_update();
    draw_chart();
}

static void pprev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    p_day_off++;
    load_day();
}

static void pnext_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (p_day_off > 0) { p_day_off--; load_day(); }
}

void scr_polar_build(lv_obj_t *parent)
{
    int base = ui_group() * CH_PER_GROUP;

    canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, cbuf, CAN_SIZE, CAN_SIZE,
                         LV_COLOR_FORMAT_ARGB8888);
    lv_obj_align(canvas, LV_ALIGN_LEFT_MID, 24, 0);

    /* hub date/time labels over the canvas centre */
    lbl_hub_t = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_hub_t, &font_units_16, 0);
    lv_obj_set_style_text_color(lbl_hub_t, COL_TEXT, 0);
    lv_obj_align_to(lbl_hub_t, canvas, LV_ALIGN_CENTER, 0, -8);
    lv_label_set_text(lbl_hub_t, "--:--");

    lbl_hub_d = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl_hub_d, &font_units_12, 0);
    lv_obj_set_style_text_color(lbl_hub_d, COL_MUTED, 0);
    lv_obj_align_to(lbl_hub_d, canvas, LV_ALIGN_CENTER, 0, 10);
    lv_label_set_text(lbl_hub_d, "--");

    /* hour labels placed cleanly around the dial rim
     * (dial centre in content coords: 204,192 / rim radius 172) */
    static const char *htxt[4] = { "00", "06", "12", "18" };
    static const int hx[4] = { 195, 382, 196, 6 };
    static const int hy[4] = { 2, 184, 368, 184 };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, htxt[i]);
        lv_obj_set_style_text_font(l, &font_units_14, 0);
        lv_obj_set_style_text_color(l, COL_MUTED, 0);
        lv_obj_set_pos(l, hx[i], hy[i]);
    }

    /* right info panel */
    lv_obj_t *info = lv_obj_create(parent);
    lv_obj_set_size(info, 356, 372);
    lv_obj_align(info, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(info, COL_PANEL, 0);
    lv_obj_set_style_border_color(info, COL_BORDER, 0);
    lv_obj_set_style_radius(info, 8, 0);
    lv_obj_set_style_pad_all(info, 10, 0);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE);

    /* day navigation header */
    lv_obj_t *bp = lv_button_create(info);
    lv_obj_set_size(bp, 34, 30);
    lv_obj_align(bp, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bp, COL_BG, 0);
    lv_obj_set_style_border_color(bp, COL_BORDER, 0);
    lv_obj_set_style_border_width(bp, 1, 0);
    lv_obj_set_style_shadow_width(bp, 0, 0);
    lv_obj_add_event_cb(bp, pprev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bpl = lv_label_create(bp);
    lv_label_set_text(bpl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bpl, COL_TEXT, 0);
    lv_obj_center(bpl);

    lbl_date = lv_label_create(info);
    lv_obj_set_style_text_font(lbl_date, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_date, COL_TEXT, 0);
    lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, 42, 7);
    lv_label_set_text(lbl_date, "Today");

    lv_obj_t *bn = lv_button_create(info);
    lv_obj_set_size(bn, 34, 30);
    lv_obj_align(bn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(bn, COL_BG, 0);
    lv_obj_set_style_border_color(bn, COL_BORDER, 0);
    lv_obj_set_style_border_width(bn, 1, 0);
    lv_obj_set_style_shadow_width(bn, 0, 0);
    lv_obj_add_event_cb(bn, pnext_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bnl = lv_label_create(bn);
    lv_label_set_text(bnl, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(bnl, COL_TEXT, 0);
    lv_obj_center(bnl);

    /* channel tiles, 2 x 4 grid - the whole tile is the tick button */
    for (int i = 0; i < CH_PER_GROUP; i++) {
        int col = i % 2;
        int row = i / 2;
        int tx = col * 171;
        int ty = 38 + row * 76;

        lv_obj_t *tile = lv_button_create(info);
        lv_obj_set_size(tile, 165, 70);
        lv_obj_align(tile, LV_ALIGN_TOP_LEFT, tx, ty);
        lv_obj_set_style_radius(tile, 6, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 6, 0);
        lv_obj_add_event_cb(tile, ptick_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        btn_ptick[i] = tile;

        lbl_ptag[i] = lv_label_create(tile);
        data_lock();
        lv_label_set_text_fmt(lbl_ptag[i], "CH%d %s", base + i + 1,
                              g_ch[base + i].tag);
        data_unlock();
        lv_obj_set_style_text_font(lbl_ptag[i], &font_units_12, 0);
        lv_obj_align(lbl_ptag[i], LV_ALIGN_TOP_LEFT, 0, 0);

        lbl_pavg[i] = lv_label_create(tile);
        lv_label_set_text(lbl_pavg[i], "-");
        lv_obj_set_style_text_font(lbl_pavg[i], &font_units_16, 0);
        lv_obj_align(lbl_pavg[i], LV_ALIGN_LEFT_MID, 0, 4);

        lbl_pmm[i] = lv_label_create(tile);
        lv_label_set_text(lbl_pmm[i], "");
        lv_obj_set_style_text_font(lbl_pmm[i], &font_units_12, 0);
        lv_obj_align(lbl_pmm[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    ptick_style_update();

    built = true;
    reload_cnt = 0;
    load_day();
}

void scr_polar_refresh(void)
{
    if (!built || !lv_obj_is_valid(canvas)) { built = false; return; }
    if (p_day_off != 0) return;

    reload_cnt++;
    if (reload_cnt >= 20) {
        /* full reload incl. statistics every 10 s */
        reload_cnt = 0;
        load_day();
    } else if ((reload_cnt & 1) == 0) {
        /* live layer: per-second samples onto the dial + hub clock */
        history_overlay_live(&pday, today_start(), ui_group());

        time_t now = time(NULL);
        struct tm tm = *localtime(&now);
        lv_label_set_text_fmt(lbl_hub_t, "%02d:%02d", tm.tm_hour, tm.tm_min);

        draw_chart();
    }
}
