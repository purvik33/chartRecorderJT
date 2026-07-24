/* scr_trend.c - strip-chart trend.
 *  - Today: fixed window of the LAST 1 HOUR, live pen every second
 *    (1 s samples from the RAM ring; storage stays at store interval)
 *  - History days (arrows): the full 24 h from storage
 *  - Y axis: absolute engineering scale (group range) by default,
 *    "%" button switches to per-channel 0-100 %
 *  - tap a line: readout shows that channel's real value
 *  - tick buttons show/hide channels (shared with polar) */
#include "ui.h"
#include "data_model.h"
#include "history.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const uint32_t series_colors[CH_PER_GROUP] = {
    0x5DCAA5, 0x85B7EB, 0xF0997B, 0xED93B1,
    0xFAC775, 0xAFA9EC, 0x97C459, 0xF09595
};

#define DISP_MAX 360

/* live window lengths (today, always ending at "now") */
static const int  zoom_sec[] = { 300, 900, 3600, 14400, 43200, 86400 };
static const char *zoom_txt[] = { "5 m", "15 m", "1 h", "4 h", "12 h", "24 h" };
#define ZOOM_COUNT 6
static int zoom = 2;           /* default 1 h */

static trend_day_t tday;
static int   day_off;          /* 0 = today, 1 = yesterday, ... */
static int   wstart_sec;
static int   reload_cnt;
/* Y axis: 0 = auto-range to the visible values, 1 = configured group
 * range, 2 = per-channel 0-100 % */
static int   y_mode = 0;
static const char *ymode_txt[] = { "auto", "abs", "%" };
static int   disp_pts;
static float glo, ghi;
static float tlo[CH_PER_GROUP], thi[CH_PER_GROUP];
static char  gunit[8];

static float disp_eng[CH_PER_GROUP][DISP_MAX];
static float disp_frac[CH_PER_GROUP][DISP_MAX];
static uint8_t have[CH_PER_GROUP][DISP_MAX];   /* sample present at point i */

static lv_obj_t *chart, *yscale, *lbl_date, *lbl_read, *lbl_zoom;
static lv_obj_t *lbl_x[7];
static lv_obj_t *btn_leg[CH_PER_GROUP];
static lv_obj_t *btn_yscale;
static lv_obj_t *pen_dot[CH_PER_GROUP];
static lv_chart_series_t *series[CH_PER_GROUP];
/* right-side values panel = the same 2x4 tile grid as the polar screen */
static lv_obj_t *vpanel, *vtag[CH_PER_GROUP], *vavg[CH_PER_GROUP],
                *vmm[CH_PER_GROUP];
static bool built;

/* chart width depends on the trend style (line = full, line+values =
 * leaves room for the values panel on the right) */
#define VP_W 340
static int chart_w(void)
{
    return (g_cfg.trend_style == 1) ? (800 - 64 - 8 - (VP_W + 8))
                                    : (800 - 64 - 8);
}

/* per-channel min / max / avg over the visible window (polar-tile style) */
static void values_update(void)
{
    if (!vpanel || g_cfg.trend_style != 1) return;
    int P = disp_pts > 0 ? disp_pts : 1;
    int base = ui_group() * CH_PER_GROUP;
    for (int c = 0; c < CH_PER_GROUP; c++) {
        if (!vavg[c]) continue;
        float mn = 0, mx = 0, sum = 0; int n = 0;
        for (int i = 0; i < P; i++) {
            if (!have[c][i]) continue;
            float v = disp_eng[c][i];
            if (!n) { mn = mx = v; }
            if (v < mn) mn = v; if (v > mx) mx = v;
            sum += v; n++;
        }
        data_lock();
        char unit[8];
        lv_snprintf(unit, sizeof(unit), "%s", g_ch[base + c].unit);
        data_unlock();
        if (n > 0) {
            lv_label_set_text_fmt(vavg[c], "%.1f %s",
                                  (double)(sum / (float)n), unit);
            lv_label_set_text_fmt(vmm[c], "Min %.1f  Max %.1f",
                                  (double)mn, (double)mx);
        } else {
            lv_label_set_text(vavg[c], "no data");
            lv_label_set_text(vmm[c], "");
        }
    }
}

static int cur_wsec(void)
{
    return day_off == 0 ? zoom_sec[zoom] : 86400;
}

static time_t day_start_for(int off)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm) - (time_t)off * 86400;
}

static int now_sod(void)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

static void leg_style_update(void)
{
    for (int i = 0; i < CH_PER_GROUP; i++) {
        if (!btn_leg[i]) continue;
        int on = ui_ch_visible(i);
        lv_color_t cc = lv_color_hex(series_colors[i]);
        lv_obj_set_style_bg_color(btn_leg[i], on ? cc : COL_PANEL, 0);
        lv_obj_set_style_border_color(btn_leg[i], on ? cc : COL_BORDER, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn_leg[i], 0),
                                    on ? COL_BG : COL_MUTED, 0);
        lv_chart_hide_series(chart, series[i], !on);
    }
}

static void pen_update(void)
{
    int w = lv_obj_get_width(chart);
    int h = lv_obj_get_height(chart);
    int P = disp_pts > 0 ? disp_pts : 1;

    for (int c = 0; c < CH_PER_GROUP; c++) {
        if (!pen_dot[c]) continue;
        int show = 0;
        if (day_off == 0 && w > 10 && ui_ch_visible(c)) {
            int last = -1;
            for (int i = P - 1; i >= 0 && i >= P - 8; i--)
                if (disp_frac[c][i] >= 0) { last = i; break; }
            if (last >= 0) {
                int px = (int)((long)last * (w - 1) / (P > 1 ? P - 1 : 1));
                int py = (h - 1) - (int)(disp_frac[c][last] * (float)(h - 1));
                lv_obj_set_pos(pen_dot[c], px - 5, py - 5);
                show = 1;
            }
        }
        if (show) lv_obj_remove_flag(pen_dot[c], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(pen_dot[c], LV_OBJ_FLAG_HIDDEN);
    }
}

static void leg_cb(lv_event_t *e)
{
    ui_ch_visible_toggle((int)(intptr_t)lv_event_get_user_data(e));
    leg_style_update();
    lv_chart_refresh(chart);
    pen_update();
}

static void redraw_window(void)
{
    int wsec = cur_wsec();
    if (day_off == 0) {
        wstart_sec = now_sod() - wsec + 1;
        if (wstart_sec < 0) wstart_sec = 0;
    } else {
        wstart_sec = 0;
    }

    int P = wsec < DISP_MAX ? wsec : DISP_MAX;
    disp_pts = P;
    lv_chart_set_point_count(chart, (uint32_t)P);

    time_t day0 = day_start_for(day_off);
    time_t now  = time(NULL);
    time_t live_lo = now - LIVE_SECS + 1;

    /* pass 1: engineering values + visible min/max for auto range */
    float vmin = 0, vmax = 0;
    int   vany = 0;

    for (int c = 0; c < CH_PER_GROUP; c++) {
        for (int i = 0; i < P; i++) {
            int s0 = wstart_sec + (int)((long)i * wsec / P);
            int s1 = wstart_sec + (int)((long)(i + 1) * wsec / P);
            if (s1 <= s0) s1 = s0 + 1;

            float sum = 0;
            int   n   = 0;

            if (day_off == 0) {
                time_t t_lo = day0 + s0;
                time_t t_hi = day0 + s1;
                if (t_lo < live_lo) t_lo = live_lo;
                if (t_hi > now + 1) t_hi = now + 1;
                for (time_t t = t_lo; t < t_hi; t++) {
                    float v;
                    if (data_live_get(ui_group() * CH_PER_GROUP + c, t, &v)) {
                        sum += v;
                        n++;
                    }
                }
            }

            if (n == 0) {
                int b0 = s0 / TREND_BUCKET_SEC;
                int b1 = (s1 + TREND_BUCKET_SEC - 1) / TREND_BUCKET_SEC;
                for (int b = b0; b < b1 && b < TREND_BUCKETS; b++) {
                    if (tday.ok[c][b]) { sum += tday.val[c][b]; n++; }
                }
            }

            if (n > 0) {
                float eng = sum / (float)n;
                disp_eng[c][i] = eng;
                have[c][i] = 1;
                if (ui_ch_visible(c)) {
                    if (!vany) { vmin = vmax = eng; vany = 1; }
                    if (eng < vmin) vmin = eng;
                    if (eng > vmax) vmax = eng;
                }
            } else {
                have[c][i] = 0;
            }
        }
    }

    /* axis range per mode */
    float alo = glo, ahi = ghi;
    if (y_mode == 0) {
        if (!vany) { alo = 0; ahi = 100; }
        else       { alo = vmin; ahi = vmax; }
        float sp = ahi - alo;
        if (sp < 1e-6f) { alo -= 1.0f; ahi += 1.0f; sp = 2.0f; }
        alo -= sp * 0.08f;
        ahi += sp * 0.08f;
        /* snap to a tidy step so the scale doesn't flicker */
        float step = 0.001f;
        while (step * 20.0f < (ahi - alo)) step *= 10.0f;
        if (step * 10.0f > (ahi - alo)) step /= 2.0f;
        alo = (float)((int)(alo / step) - (alo < 0 ? 1 : 0)) * step;
        ahi = (float)((int)(ahi / step) + 1) * step;
    }

    if (y_mode == 2) lv_scale_set_range(yscale, 0, 100);
    else             lv_scale_set_range(yscale, (int32_t)alo, (int32_t)ahi);

    /* pass 2: plot with the chosen scaling */
    for (int c = 0; c < CH_PER_GROUP; c++) {
        float lo   = (y_mode == 2) ? tlo[c] : alo;
        float hi   = (y_mode == 2) ? thi[c] : ahi;
        float span = (hi - lo) != 0 ? (hi - lo) : 1.0f;

        for (int i = 0; i < P; i++) {
            if (have[c][i]) {
                float pct = (disp_eng[c][i] - lo) / span * 1000.0f;
                if (pct < 0)    pct = 0;
                if (pct > 1000) pct = 1000;
                disp_frac[c][i] = pct / 1000.0f;
                lv_chart_set_value_by_id(chart, series[c], (uint32_t)i,
                                         (int32_t)pct);
            } else {
                disp_frac[c][i] = -1.0f;
                lv_chart_set_value_by_id(chart, series[c], (uint32_t)i,
                                         LV_CHART_POINT_NONE);
            }
        }
    }

    for (int i = 0; i < 7; i++) {
        int sec = wstart_sec + (wsec * i) / 6;
        if (sec > 86400) sec = 86400;
        lv_label_set_text_fmt(lbl_x[i], "%02d:%02d",
                              sec / 3600, (sec % 3600) / 60);
    }

    lv_chart_refresh(chart);
    pen_update();
    values_update();
}

static void load_day(void)
{
    time_t ds = day_start_for(day_off);
    int base = ui_group() * CH_PER_GROUP;

    history_load(ds, ui_group(), &tday);

    data_lock();
    glo = g_ch[base].lo;
    ghi = g_ch[base].hi;
    for (int c = 0; c < CH_PER_GROUP; c++) {
        tlo[c] = g_ch[base + c].lo;
        thi[c] = g_ch[base + c].hi;
        if (g_ch[base + c].lo < glo) glo = g_ch[base + c].lo;
        if (g_ch[base + c].hi > ghi) ghi = g_ch[base + c].hi;
    }
    lv_snprintf(gunit, sizeof(gunit), "%s", g_ch[base].unit);
    data_unlock();
    if (ghi <= glo) ghi = glo + 1.0f;

    if (day_off == 0) {
        lv_label_set_text(lbl_date, "Today");
    } else {
        struct tm dt = *localtime(&ds);
        lv_label_set_text_fmt(lbl_date, "%04d-%02d-%02d",
                              dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
    }
    redraw_window();
}

static void chart_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    uint32_t id = lv_chart_get_pressed_point(chart);
    if (id == LV_CHART_POINT_NONE || (int)id >= DISP_MAX) return;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t coords;
    lv_obj_get_coords(chart, &coords);
    int h = lv_area_get_height(&coords);
    if (h <= 0) return;
    float frac = (float)(coords.y2 - p.y) / (float)h;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    int wsec = cur_wsec();
    int P = disp_pts > 0 ? disp_pts : 1;
    int sec = wstart_sec + (int)((long)id * wsec / P);

    /* snap to the nearest visible trace and report its REAL value */
    int   best  = -1;
    float bestd = 0.10f;
    for (int c = 0; c < CH_PER_GROUP; c++) {
        if (!ui_ch_visible(c) || disp_frac[c][id] < 0) continue;
        float d = disp_frac[c][id] - frac;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = c; }
    }

    if (best >= 0) {
        data_lock();
        lv_label_set_text_fmt(lbl_read, "%s  %02d:%02d:%02d   %.1f %s",
                              g_ch[ui_group() * CH_PER_GROUP + best].tag,
                              sec / 3600, (sec % 3600) / 60, sec % 60,
                              (double)disp_eng[best][id],
                              g_ch[ui_group() * CH_PER_GROUP + best].unit);
        data_unlock();
    } else {
        lv_label_set_text_fmt(lbl_read, "%02d:%02d:%02d",
                              sec / 3600, (sec % 3600) / 60, sec % 60);
    }
    lv_obj_set_style_text_color(lbl_read, COL_TEXT, 0);
}

static void yscale_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    y_mode = (y_mode + 1) % 3;
    lv_label_set_text(lv_obj_get_child(btn_yscale, 0), ymode_txt[y_mode]);
    load_day();
}

static void zoom_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int nz = zoom + dir;
    if (nz < 0 || nz >= ZOOM_COUNT) return;
    zoom = nz;
    lv_label_set_text(lbl_zoom, zoom_txt[zoom]);
    redraw_window();
}

static void prev_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    day_off++;
    load_day();
}

static void next_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (day_off == 0) return;
    day_off--;
    load_day();
}

static lv_obj_t *nav_btn(lv_obj_t *parent, const char *sym,
                         lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 42, 36);
    lv_obj_set_style_bg_color(b, COL_PANEL, 0);
    lv_obj_set_style_border_color(b, COL_BORDER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_center(l);
    return b;
}

void scr_trend_build(lv_obj_t *parent)
{
    int base = ui_group() * CH_PER_GROUP;

    /* ---- top bar ---- */
    lv_obj_t *top = lv_obj_create(parent);
    lv_obj_set_size(top, LV_PCT(100), 44);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 2, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    /* left cluster starts after the Y-scale column (x 64) - the area
     * above the scale numbers stays empty */
    lv_obj_t *bp = nav_btn(top, LV_SYMBOL_LEFT, prev_cb, NULL);
    lv_obj_align(bp, LV_ALIGN_LEFT_MID, 66, 0);

    lbl_date = lv_label_create(top);
    lv_obj_set_style_text_font(lbl_date, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_date, COL_TEXT, 0);
    lv_obj_align(lbl_date, LV_ALIGN_LEFT_MID, 116, 0);
    lv_obj_set_width(lbl_date, 86);

    lv_obj_t *bn = nav_btn(top, LV_SYMBOL_RIGHT, next_cb, NULL);
    lv_obj_align(bn, LV_ALIGN_LEFT_MID, 206, 0);

    lv_obj_t *bzo = nav_btn(top, LV_SYMBOL_MINUS, zoom_cb, (void *)(intptr_t)1);
    lv_obj_align(bzo, LV_ALIGN_LEFT_MID, 262, 0);
    lbl_zoom = lv_label_create(top);
    lv_label_set_text(lbl_zoom, zoom_txt[zoom]);
    lv_obj_set_style_text_font(lbl_zoom, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_zoom, COL_TEXT, 0);
    lv_obj_align(lbl_zoom, LV_ALIGN_LEFT_MID, 310, 0);
    lv_obj_set_width(lbl_zoom, 38);
    lv_obj_t *bzi = nav_btn(top, LV_SYMBOL_PLUS, zoom_cb, (void *)(intptr_t)-1);
    lv_obj_align(bzi, LV_ALIGN_LEFT_MID, 352, 0);

    btn_yscale = nav_btn(top, ymode_txt[y_mode], yscale_cb, NULL);
    lv_obj_set_width(btn_yscale, 52);
    lv_obj_align(btn_yscale, LV_ALIGN_LEFT_MID, 408, 0);

    /* right corner: channel ticks, with the readout value beside them */
    for (int i = 0; i < CH_PER_GROUP; i++) {
        lv_obj_t *b = lv_button_create(top);
        lv_obj_set_size(b, 34, 36);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -6 - (7 - i) * 37, 0);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_add_event_cb(b, leg_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text_fmt(l, "%d", base + i + 1);
        lv_obj_set_style_text_font(l, &font_units_16, 0);
        lv_obj_center(l);
        btn_leg[i] = b;
    }

    lbl_read = lv_label_create(top);
    lv_label_set_text(lbl_read, "");
    lv_obj_set_style_text_font(lbl_read, &font_units_14, 0);
    lv_obj_set_style_text_color(lbl_read, COL_MUTED, 0);
    lv_obj_align(lbl_read, LV_ALIGN_RIGHT_MID, -310, 0);

    /* ---- Y scale ---- */
    int chart_h = 480 - 40 - 56 - 44 - 24;
    yscale = lv_scale_create(parent);
    lv_scale_set_mode(yscale, LV_SCALE_MODE_VERTICAL_LEFT);
    lv_obj_set_size(yscale, 60, chart_h);
    lv_obj_align(yscale, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_scale_set_total_tick_count(yscale, 11);
    lv_scale_set_major_tick_every(yscale, 2);
    lv_scale_set_label_show(yscale, true);
    lv_obj_set_style_text_color(yscale, COL_MUTED, 0);
    lv_obj_set_style_text_font(yscale, &font_units_12, 0);
    lv_obj_set_style_line_color(yscale, COL_BORDER, 0);
    lv_obj_set_style_line_color(yscale, COL_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_line_color(yscale, COL_MUTED, LV_PART_INDICATOR);

    /* ---- chart ---- */
    chart = lv_chart_create(parent);
    lv_obj_set_size(chart, chart_w(), chart_h);
    lv_obj_align(chart, LV_ALIGN_TOP_LEFT, 64, 44);
    lv_obj_set_style_bg_color(chart, COL_PANEL, 0);
    lv_obj_set_style_border_color(chart, COL_BORDER, 0);
    lv_obj_set_style_line_color(chart, COL_BORDER, LV_PART_MAIN);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_div_line_count(chart, 5, 7);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_add_event_cb(chart, chart_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(chart, chart_event_cb, LV_EVENT_PRESSING, NULL);

    for (int i = 0; i < CH_PER_GROUP; i++)
        series[i] = lv_chart_add_series(
            chart, lv_color_hex(series_colors[i]), LV_CHART_AXIS_PRIMARY_Y);
    leg_style_update();

    for (int i = 0; i < CH_PER_GROUP; i++) {
        lv_obj_t *d = lv_obj_create(chart);
        lv_obj_set_size(d, 10, 10);
        lv_obj_set_style_radius(d, 5, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(series_colors[i]), 0);
        lv_obj_set_style_border_color(d, COL_TEXT, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        pen_dot[i] = d;
    }

    /* ---- values panel (Trend style: line + values) - same 2x4 tile
     * grid as the polar screen (tag / big value / Min-Max, colour-coded) */
    vpanel = NULL;
    for (int i = 0; i < CH_PER_GROUP; i++) { vtag[i] = vavg[i] = vmm[i] = NULL; }
    if (g_cfg.trend_style == 1) {
        vpanel = lv_obj_create(parent);
        lv_obj_set_size(vpanel, VP_W, chart_h);
        lv_obj_align(vpanel, LV_ALIGN_TOP_LEFT, 64 + chart_w() + 8, 44);
        lv_obj_set_style_bg_color(vpanel, COL_PANEL, 0);
        lv_obj_set_style_border_color(vpanel, COL_BORDER, 0);
        lv_obj_set_style_border_width(vpanel, 1, 0);
        lv_obj_set_style_radius(vpanel, 8, 0);
        lv_obj_set_style_pad_all(vpanel, 8, 0);
        lv_obj_set_layout(vpanel, LV_LAYOUT_NONE);   /* absolute 2x4 grid */
        lv_obj_remove_flag(vpanel, LV_OBJ_FLAG_SCROLLABLE);
        int tw = (VP_W - 16 - 6) / 2;             /* two tiles across */
        int th = (chart_h - 16 - 3 * 6) / 4;      /* four tiles down  */
        for (int i = 0; i < CH_PER_GROUP; i++) {
            lv_color_t cc = lv_color_hex(series_colors[i]);
            lv_obj_t *tile = lv_obj_create(vpanel);
            lv_obj_set_size(tile, tw, th);
            lv_obj_set_pos(tile, (i % 2) * (tw + 6), (i / 2) * (th + 6));
            lv_obj_set_style_bg_color(tile, lv_color_mix(cc, COL_PANEL, 30), 0);
            lv_obj_set_style_border_color(tile, cc, 0);
            lv_obj_set_style_border_width(tile, 2, 0);
            lv_obj_set_style_radius(tile, 6, 0);
            lv_obj_set_style_pad_all(tile, 6, 0);
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

            vtag[i] = lv_label_create(tile);
            data_lock();
            lv_label_set_text_fmt(vtag[i], "CH%d %s", base + i + 1,
                                  g_ch[base + i].tag);
            data_unlock();
            lv_obj_set_style_text_font(vtag[i], &font_units_12, 0);
            lv_obj_set_style_text_color(vtag[i], cc, 0);
            lv_obj_align(vtag[i], LV_ALIGN_TOP_LEFT, 0, 0);

            vavg[i] = lv_label_create(tile);
            lv_label_set_text(vavg[i], "-");
            lv_obj_set_style_text_font(vavg[i], &font_units_16, 0);
            lv_obj_set_style_text_color(vavg[i], COL_TEXT, 0);
            lv_obj_align(vavg[i], LV_ALIGN_LEFT_MID, 0, 4);

            vmm[i] = lv_label_create(tile);
            lv_label_set_text(vmm[i], "");
            lv_obj_set_style_text_font(vmm[i], &font_units_12, 0);
            lv_obj_set_style_text_color(vmm[i], COL_MUTED, 0);
            lv_obj_align(vmm[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
    }

    /* ---- X labels ---- */
    lv_obj_t *xrow = lv_obj_create(parent);
    lv_obj_set_size(xrow, chart_w(), 22);
    lv_obj_align(xrow, LV_ALIGN_BOTTOM_LEFT, 64, 0);
    lv_obj_set_style_bg_opa(xrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(xrow, 0, 0);
    lv_obj_set_style_pad_all(xrow, 0, 0);
    lv_obj_set_flex_flow(xrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(xrow, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(xrow, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 7; i++) {
        lbl_x[i] = lv_label_create(xrow);
        lv_label_set_text(lbl_x[i], "--:--");
        lv_obj_set_style_text_font(lbl_x[i], &font_units_12, 0);
        lv_obj_set_style_text_color(lbl_x[i], COL_MUTED, 0);
    }

    built = true;
    reload_cnt = 0;
    load_day();
}

void scr_trend_refresh(void)
{
    if (!built || !lv_obj_is_valid(chart)) { built = false; return; }
    if (day_off != 0) return;

    reload_cnt++;

    for (int i = 0; i < CH_PER_GROUP; i++)
        if (pen_dot[i])
            lv_obj_set_style_opa(pen_dot[i],
                (reload_cnt & 1) ? LV_OPA_COVER : LV_OPA_50, 0);

    if (reload_cnt >= 20) {
        reload_cnt = 0;
        load_day();
    } else if ((reload_cnt & 1) == 0) {
        redraw_window();
    }
}
