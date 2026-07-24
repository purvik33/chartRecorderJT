#include "export.h"
#include "alarm.h"
#include "pdf.h"
#include "data_model.h"
#include "config.h"
#include "version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>

int usb_find(char *out, size_t n)
{
    DWORD mask = GetLogicalDrives();
    for (int i = 2; i < 26; i++) {          /* skip A: B: */
        if (!(mask & (1u << i))) continue;
        char root[8];
        snprintf(root, sizeof(root), "%c:\\", 'A' + i);
        if (GetDriveTypeA(root) == DRIVE_REMOVABLE) {
            snprintf(out, n, "%c:\\", 'A' + i);
            return 1;
        }
    }
    return 0;
}

#else
#include <dirent.h>
#include <unistd.h>

/* USB sticks appear under /media/<user>/<label> or /run/media/... */
static int scan_mounts(const char *base, char *out, size_t n, int depth)
{
    DIR *d = opendir(base);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", base, e->d_name);
        if (depth > 0 && scan_mounts(path, out, n, depth - 1)) {
            closedir(d);
            return 1;
        }
        if (depth == 0 && access(path, W_OK) == 0) {
            snprintf(out, n, "%s/", path);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

int usb_find(char *out, size_t n)
{
    if (scan_mounts("/media", out, n, 1))     return 1;
    if (scan_mounts("/run/media", out, n, 1)) return 1;
    if (scan_mounts("/media", out, n, 0))     return 1;
    return 0;
}
#endif

static time_t parse_row_time(const char *line)
{
    struct tm tm = {0};
    if (sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return (time_t)-1;
    tm.tm_year -= 1900;
    tm.tm_mon  -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

int export_range(time_t t0, time_t t1, char *msg, size_t msglen)
{
    if (t1 < t0) { snprintf(msg, msglen, "End is before start"); return -1; }

    char usb[128];
    if (!usb_find(usb, sizeof(usb))) {
        snprintf(msg, msglen, "No USB drive found");
        return -1;
    }

    struct tm a = *localtime(&t0);
    char dest[256];
    struct tm b = *localtime(&t1);
    snprintf(dest, sizeof(dest),
             "%sdata_%04d%02d%02d-%02d%02d_%04d%02d%02d-%02d%02d.csv",
             usb,
             a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min,
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);

    FILE *out = fopen(dest, "w");
    if (!out) { snprintf(msg, msglen, "Cannot write to USB drive"); return -1; }

    int rows = 0, header_done = 0;
    char line[4096];

    /* walk the day files covering the requested span */
    for (time_t day = t0 - 86400; day <= t1 + 86400; day += 86400) {
        struct tm dt = *localtime(&day);
        char path[64];
        snprintf(path, sizeof(path), "logs/%04d-%02d-%02d.csv",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
        FILE *in = fopen(path, "r");
        if (!in) continue;

        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "timestamp", 9)) {
                if (!header_done) { fputs(line, out); header_done = 1; }
                continue;
            }
            time_t t = parse_row_time(line);
            if (t == (time_t)-1) continue;
            if (t >= t0 && t <= t1) { fputs(line, out); rows++; }
        }
        fclose(in);
    }
    fclose(out);

    if (rows == 0) {
        remove(dest);
        snprintf(msg, msglen, "No data in the selected period");
        return 0;
    }
    snprintf(msg, msglen, "Exported %d records to USB", rows);
    return rows;
}

int export_events_range(time_t t0, time_t t1, char *msg, size_t msglen)
{
    if (t1 < t0) { snprintf(msg, msglen, "End is before start"); return -1; }

    char usb[128];
    if (!usb_find(usb, sizeof(usb))) {
        snprintf(msg, msglen, "No USB drive found");
        return -1;
    }

    struct tm a = *localtime(&t0);
    struct tm b = *localtime(&t1);
    char dest[256];
    snprintf(dest, sizeof(dest),
             "%sevents_%04d%02d%02d-%02d%02d_%04d%02d%02d-%02d%02d.csv",
             usb,
             a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min,
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);

    FILE *out = fopen(dest, "w");
    if (!out) { snprintf(msg, msglen, "Cannot write to USB drive"); return -1; }

    int rows = 0, header_done = 0;
    char line[300];

    for (time_t day = t0 - 86400; day <= t1 + 86400; day += 86400) {
        struct tm dt = *localtime(&day);
        char path[64];
        snprintf(path, sizeof(path), "logs/events-%04d-%02d-%02d.csv",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
        FILE *in = fopen(path, "r");
        if (!in) continue;

        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "timestamp", 9)) {
                if (!header_done) { fputs(line, out); header_done = 1; }
                continue;
            }
            time_t t = parse_row_time(line);
            if (t == (time_t)-1) continue;
            if (t >= t0 && t <= t1) { fputs(line, out); rows++; }
        }
        fclose(in);
    }
    fclose(out);

    if (rows == 0) {
        remove(dest);
        snprintf(msg, msglen, "No events in the selected period");
        return 0;
    }
    snprintf(msg, msglen, "Exported %d events to USB", rows);
    return rows;
}

#define EXP_ALARM_MAX 500
static alarm_rec_t exp_recs[EXP_ALARM_MAX];

int export_alarms_range(time_t t0, time_t t1, char *msg, size_t msglen)
{
    if (t1 < t0) { snprintf(msg, msglen, "End is before start"); return -1; }

    char usb[128];
    if (!usb_find(usb, sizeof(usb))) {
        snprintf(msg, msglen, "No USB drive found");
        return -1;
    }

    int n = alarm_records_load(t0, t1, exp_recs, EXP_ALARM_MAX);
    if (n == 0) {
        snprintf(msg, msglen, "No alarms in the selected period");
        return 0;
    }

    struct tm a = *localtime(&t0);
    struct tm b = *localtime(&t1);
    char dest[256];
    snprintf(dest, sizeof(dest),
             "%salarms_%04d%02d%02d-%02d%02d_%04d%02d%02d-%02d%02d.csv",
             usb,
             a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min,
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);

    FILE *out = fopen(dest, "w");
    if (!out) { snprintf(msg, msglen, "Cannot write to USB drive"); return -1; }

    fprintf(out, "channel,tag,type,value,generated,acknowledged,resolved\n");
    for (int i = 0; i < n; i++) {
        alarm_rec_t *r = &exp_recs[i];
        fprintf(out, "%s,%s,%s,%.3f,%s,%s,%s\n",
                r->ch, r->tag, r->type, (double)r->value,
                r->set_ts,
                r->ack_ts[0] ? r->ack_ts : "-",
                r->clr_ts[0] ? r->clr_ts : "ACTIVE");
    }
    fclose(out);

    snprintf(msg, msglen, "Exported %d alarms to USB", n);
    return n;
}

/* ---- PDF report (same content as the web dashboard export) ------------- */

#define RC_MAXCH   CH_TOTAL
#define RC_CHARTX  360

static const int rc_pal[8][3] = {
    { 93,202,165},{133,183,235},{240,153,123},{237,147,177},
    {250,199,117},{175,169,236},{151,196, 89},{240,149,149}
};

static char   rc_tag[RC_MAXCH][12];
static char   rc_unit[RC_MAXCH][8];
static float  rc_lo[RC_MAXCH], rc_hi[RC_MAXCH];
static float  rc_min[RC_MAXCH], rc_max[RC_MAXCH];
static double rc_sum[RC_MAXCH];
static long   rc_cnt[RC_MAXCH];
static float  rc_cx[RC_CHARTX];
static float  rc_cv[RC_CHARTX][RC_MAXCH];
static char   rc_days[400][11];

static int rc_split(char *line, char **f, int maxf)
{
    int n = 0; f[n++] = line;
    for (char *p = line; *p && n < maxf; p++)
        if (*p == ',') { *p = 0; f[n++] = p + 1; }
    return n;
}
static int rc_isnum(const char *s)
{
    return !strcmp(s, "OK") || !strcmp(s, "HI") || !strcmp(s, "LO");
}
static void rc_rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = 0;
}
static void rc_fmt(float v, char *out, int n)
{
    if (fabsf(v) >= 1000.0f || v == (float)(long)v) snprintf(out, n, "%ld", (long)v);
    else snprintf(out, n, "%.1f", (double)v);
}
static float rc_inforow(pdf_t *pd, float x0, float x1, float y,
                        const char *k1, const char *v1,
                        const char *k2, const char *v2)
{
    pdf_text(pd, x0, y + 11, 9, 0, 110, 110, 110, k1);
    pdf_text(pd, x0 + 78, y + 11, 9, 0, 20, 20, 20, v1);
    if (k2) {
        pdf_text(pd, x0 + 260, y + 11, 9, 0, 110, 110, 110, k2);
        pdf_text(pd, x0 + 335, y + 11, 9, 0, 20, 20, 20, v2);
    }
    pdf_line(pd, x0, y + 16, x1, y + 16, 0.4f, 210);
    return y + 18;
}

int export_report_pdf(time_t t0, time_t t1, char *msg, size_t msglen)
{
    if (t1 < t0) { snprintf(msg, msglen, "End is before start"); return -1; }
    char usb[128];
    if (!usb_find(usb, sizeof(usb))) {
        snprintf(msg, msglen, "No USB drive found"); return -1;
    }

    int nch = g_cfg.cards * CH_PER_GROUP;
    if (nch < CH_PER_GROUP) nch = CH_PER_GROUP;
    if (nch > RC_MAXCH) nch = RC_MAXCH;

    data_lock();
    for (int i = 0; i < nch; i++) {
        snprintf(rc_tag[i],  sizeof(rc_tag[i]),  "%s", g_ch[i].tag);
        snprintf(rc_unit[i], sizeof(rc_unit[i]), "%s", g_ch[i].unit);
        rc_lo[i] = g_ch[i].lo; rc_hi[i] = g_ch[i].hi;
        if (!(rc_hi[i] > rc_lo[i])) { rc_lo[i] = 0; rc_hi[i] = 1; }
        rc_min[i] = 1e30f; rc_max[i] = -1e30f; rc_sum[i] = 0; rc_cnt[i] = 0;
    }
    data_unlock();

    /* distinct calendar days in the range */
    int nd = 0; char prev[11] = "";
    for (time_t d = t0; d <= t1 + 86400 && nd < 400; d += 86400) {
        struct tm dt = *localtime(&d);
        char ds[11];
        snprintf(ds, sizeof(ds), "%04d-%02d-%02d",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
        if (strcmp(ds, prev)) { strcpy(rc_days[nd++], ds); strcpy(prev, ds); }
    }

    char line[4096]; char *f[1 + 2 * RC_MAXCH];
    long total = 0;

    /* pass 1: per-channel min/max/avg + record count */
    for (int di = 0; di < nd; di++) {
        char path[64];
        snprintf(path, sizeof(path), "logs/%s.csv", rc_days[di]);
        FILE *in = fopen(path, "r"); if (!in) continue;
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "timestamp", 9)) continue;
            time_t t = parse_row_time(line);
            if (t == (time_t)-1 || t < t0 || t > t1) continue;
            rc_rstrip(line);
            int nf = rc_split(line, f, 1 + 2 * nch);
            for (int i = 0; i < nch; i++) {
                int sc = 2 + 2 * i; if (sc >= nf) break;
                if (!rc_isnum(f[sc])) continue;
                float v = (float)atof(f[1 + 2 * i]);
                if (v < rc_min[i]) rc_min[i] = v;
                if (v > rc_max[i]) rc_max[i] = v;
                rc_sum[i] += v; rc_cnt[i]++;
            }
            total++;
        }
        fclose(in);
    }
    if (total == 0) {
        snprintf(msg, msglen, "No data in the selected period"); return 0;
    }

    /* pass 2: collect down-sampled chart points across the whole range */
    long stepc = total / RC_CHARTX; if (stepc < 1) stepc = 1;
    double span = (double)(t1 - t0); if (span <= 0) span = 1;
    int cn = 0; long ri = 0;
    for (int di = 0; di < nd && cn < RC_CHARTX; di++) {
        char path[64];
        snprintf(path, sizeof(path), "logs/%s.csv", rc_days[di]);
        FILE *in = fopen(path, "r"); if (!in) continue;
        while (fgets(line, sizeof(line), in) && cn < RC_CHARTX) {
            if (!strncmp(line, "timestamp", 9)) continue;
            time_t t = parse_row_time(line);
            if (t == (time_t)-1 || t < t0 || t > t1) continue;
            if ((ri++ % stepc) != 0) continue;
            rc_rstrip(line);
            int nf = rc_split(line, f, 1 + 2 * nch);
            rc_cx[cn] = (float)(((double)(t - t0)) / span);
            for (int i = 0; i < nch; i++) {
                int sc = 2 + 2 * i;
                rc_cv[cn][i] = (sc < nf && rc_isnum(f[sc]))
                             ? (float)atof(f[1 + 2 * i]) : NAN;
            }
            cn++;
        }
        fclose(in);
    }

    pdf_t *pd = pdf_new();
    if (!pd) { snprintf(msg, msglen, "Out of memory"); return -1; }

    const float PW = 595, PH = 842, M = 40;
    const float LW = 842, LH = 595;

    time_t now = time(NULL); struct tm nt = *localtime(&now);
    char rid[40];
    snprintf(rid, sizeof(rid), "PR40-%04d%02d%02d-%02d%02d%02d",
             nt.tm_year + 1900, nt.tm_mon + 1, nt.tm_mday,
             nt.tm_hour, nt.tm_min, nt.tm_sec);
    struct tm a = *localtime(&t0), b = *localtime(&t1);
    char per[80], gen[40], sbuf[24], dbuf[16];
    snprintf(per, sizeof(per),
             "%04d-%02d-%02d %02d:%02d  to  %04d-%02d-%02d %02d:%02d",
             a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min,
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);
    snprintf(gen, sizeof(gen), "%04d-%02d-%02d %02d:%02d:%02d",
             nt.tm_year + 1900, nt.tm_mon + 1, nt.tm_mday,
             nt.tm_hour, nt.tm_min, nt.tm_sec);
    snprintf(sbuf, sizeof(sbuf), "%ld", total);
    snprintf(dbuf, sizeof(dbuf), "%d", nd);
    const char *brand = g_cfg.brand[0] ? g_cfg.brand : "JETPACE Technologies";
    const char *model = g_cfg.model[0] ? g_cfg.model : "PR-40 Paperless Recorder";

    /* ---------- page 1: letterhead + info + summary (portrait) ---------- */
    pdf_page(pd, PW, PH);
    pdf_fill(pd, M, M, 40, 40, 13, 27, 42);
    { float wl[10] = { M+7,M+30, M+16,M+20, M+22,M+25, M+30,M+12, M+37,M+21 };
      pdf_polyline(pd, wl, 5, 2.2f, 79, 195, 247); }
    pdf_text(pd, M + 52, M + 16, 15, 1, 13, 27, 42, brand);
    pdf_text(pd, M + 52, M + 32, 9, 0, 100, 100, 100, model);
    pdf_text_right(pd, PW - M, M + 14, 15, 1, 42, 122, 176, "DATA REPORT");
    pdf_fill(pd, M, M + 46, PW - 2 * M, 1.5f, 13, 27, 42);

    float y = M + 62;
    pdf_text(pd, M, y, 12, 1, 13, 27, 42, "Report information"); y += 16;
    y = rc_inforow(pd, M, PW - M, y, "Report ID", rid, "Firmware", "v" FW_VERSION);
    y = rc_inforow(pd, M, PW - M, y, "Instrument", model, "Records", sbuf);
    y = rc_inforow(pd, M, PW - M, y, "Generated", gen, "Days", dbuf);
    y = rc_inforow(pd, M, PW - M, y, "Period", per, NULL, NULL);

    y += 6;
    pdf_text(pd, M, y, 12, 1, 13, 27, 42, "Summary"); y += 14;
    pdf_fill(pd, M, y, PW - 2 * M, 16, 238, 243, 247);
    pdf_text(pd, M + 2, y + 11, 8, 1, 13, 27, 42, "Ch");
    pdf_text(pd, M + 40, y + 11, 8, 1, 13, 27, 42, "Tag");
    pdf_text(pd, M + 150, y + 11, 8, 1, 13, 27, 42, "Unit");
    pdf_text_right(pd, M + 300, y + 11, 8, 1, 13, 27, 42, "Min");
    pdf_text_right(pd, M + 370, y + 11, 8, 1, 13, 27, 42, "Max");
    pdf_text_right(pd, M + 440, y + 11, 8, 1, 13, 27, 42, "Avg");
    pdf_text_right(pd, PW - M, y + 11, 8, 1, 13, 27, 42, "Samples");
    y += 16;
    for (int i = 0; i < nch; i++) {
        char cb[8]; snprintf(cb, sizeof(cb), "CH%d", i + 1);
        pdf_text(pd, M + 2, y + 11, 8, 0, 60, 60, 60, cb);
        pdf_text(pd, M + 40, y + 11, 8, 0, 20, 20, 20, rc_tag[i]);
        pdf_text(pd, M + 150, y + 11, 8, 0, 120, 120, 120, rc_unit[i]);
        if (rc_cnt[i] > 0) {
            char mn[16], mx[16], av[16], sm[16];
            rc_fmt(rc_min[i], mn, sizeof(mn));
            rc_fmt(rc_max[i], mx, sizeof(mx));
            rc_fmt((float)(rc_sum[i] / (double)rc_cnt[i]), av, sizeof(av));
            snprintf(sm, sizeof(sm), "%ld", rc_cnt[i]);
            pdf_text_right(pd, M + 300, y + 11, 8, 0, 20, 20, 20, mn);
            pdf_text_right(pd, M + 370, y + 11, 8, 0, 20, 20, 20, mx);
            pdf_text_right(pd, M + 440, y + 11, 8, 0, 20, 20, 20, av);
            pdf_text_right(pd, PW - M, y + 11, 8, 0, 20, 20, 20, sm);
        } else {
            pdf_text_right(pd, PW - M, y + 11, 8, 0, 150, 150, 150, "no data");
        }
        pdf_line(pd, M, y + 16, PW - M, y + 16, 0.3f, 220);
        y += 16;
    }

    /* ---------- trend page (landscape) ---------- */
    pdf_page(pd, LW, LH);
    pdf_text(pd, M, M + 12, 13, 1, 13, 27, 42,
             "Trend  (normalized to each channel's range)");
    float cx0 = M + 34, cy0 = M + 24, cw = LW - M - cx0, chh = LH - M - 60 - cy0;
    pdf_rect(pd, cx0, cy0, cw, chh, 0.8f, 150);
    for (int k = 0; k <= 5; k++) {
        float yy = cy0 + chh * k / 5.0f;
        if (k > 0 && k < 5) pdf_line(pd, cx0, yy, cx0 + cw, yy, 0.4f, 225);
        char pc[8]; snprintf(pc, sizeof(pc), "%d%%", 100 - 20 * k);
        pdf_text_right(pd, cx0 - 4, yy + 3, 8, 0, 120, 120, 120, pc);
    }
    {
        static float pts[RC_CHARTX * 2];
        for (int i = 0; i < nch; i++) {
            int m = 0;
            for (int k = 0; k < cn; k++) {
                float v = rc_cv[k][i]; if (isnan(v)) continue;
                float fr = (v - rc_lo[i]) / (rc_hi[i] - rc_lo[i]);
                if (fr < 0) fr = 0; if (fr > 1) fr = 1;
                pts[2 * m] = cx0 + cw * rc_cx[k];
                pts[2 * m + 1] = cy0 + chh * (1.0f - fr);
                m++;
            }
            if (m >= 2)
                pdf_polyline(pd, pts, m, 1.2f,
                             rc_pal[i%8][0], rc_pal[i%8][1], rc_pal[i%8][2]);
        }
    }
    for (int k = 0; k <= 6; k++) {
        float fr = k / 6.0f; time_t tt = t0 + (time_t)(fr * (double)(t1 - t0));
        struct tm xt = *localtime(&tt); char xl[20];
        snprintf(xl, sizeof(xl), "%02d-%02d %02d:%02d",
                 xt.tm_mon + 1, xt.tm_mday, xt.tm_hour, xt.tm_min);
        pdf_text(pd, cx0 + cw * fr - 24, cy0 + chh + 14, 7.5f, 0,
                 120, 120, 120, xl);
    }
    {
        float lx = M, ly = cy0 + chh + 34;
        for (int i = 0; i < nch; i++) {
            char lg[24]; snprintf(lg, sizeof(lg), "CH%d %s", i + 1, rc_tag[i]);
            pdf_fill(pd, lx, ly - 8, 9, 9,
                     rc_pal[i%8][0], rc_pal[i%8][1], rc_pal[i%8][2]);
            pdf_text(pd, lx + 12, ly, 7.5f, 0, 40, 40, 40, lg);
            lx += 12 + pdf_str_w(7.5f, 0, lg) + 16;
            if (lx > LW - 130) { lx = M; ly += 13; }
        }
    }

    /* ---------- per-day readings pages (landscape) ---------- */
    float timeW = 66;
    float colw = (LW - 2 * M - timeW) / (float)nch;
    if (colw > 60) colw = 60; if (colw < 24) colw = 24;
    float tblw = timeW + colw * nch;
    float fs = colw < 34 ? 6.5f : 7.5f;
    const int ROWS_PP = 36;

    for (int di = 0; di < nd; di++) {
        char path[64];
        snprintf(path, sizeof(path), "logs/%s.csv", rc_days[di]);
        FILE *in = fopen(path, "r"); if (!in) continue;
        long drows = 0;
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "timestamp", 9)) continue;
            time_t t = parse_row_time(line);
            if (t != (time_t)-1 && t >= t0 && t <= t1) drows++;
        }
        fclose(in);
        if (drows == 0) continue;
        long dstep = drows / 240; if (dstep < 1) dstep = 1;

        in = fopen(path, "r"); if (!in) continue;
        int rowc = 0, page_open = 0; long idx = 0; float ry = 0;
        while (fgets(line, sizeof(line), in)) {
            if (!strncmp(line, "timestamp", 9)) continue;
            time_t t = parse_row_time(line);
            if (t == (time_t)-1 || t < t0 || t > t1) continue;
            if ((idx++ % dstep) != 0) continue;
            if (!page_open || rowc >= ROWS_PP) {
                pdf_page(pd, LW, LH); page_open = 1; rowc = 0;
                pdf_text(pd, M, M + 12, 12, 1, 13, 27, 42, rc_days[di]);
                char sub[48];
                snprintf(sub, sizeof(sub), "%ld records%s",
                         drows, dstep > 1 ? "  (sampled)" : "");
                pdf_text(pd, M + 120, M + 12, 9, 0, 120, 120, 120, sub);
                ry = M + 24;
                pdf_fill(pd, M, ry, tblw, 14, 238, 243, 247);
                pdf_text(pd, M + 2, ry + 10, fs, 1, 13, 27, 42, "Time");
                for (int i = 0; i < nch; i++) {
                    char h[10]; snprintf(h, sizeof(h), "CH%d", i + 1);
                    pdf_text_right(pd, M + timeW + colw * (i + 1) - 2,
                                   ry + 10, fs, 1, 13, 27, 42, h);
                }
                ry += 14;
            }
            struct tm rt = *localtime(&t); char tbuf[12];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                     rt.tm_hour, rt.tm_min, rt.tm_sec);
            rc_rstrip(line);
            int nf = rc_split(line, f, 1 + 2 * nch);
            pdf_text(pd, M + 2, ry + 10, fs, 0, 60, 60, 60, tbuf);
            for (int i = 0; i < nch; i++) {
                int sc = 2 + 2 * i; char cell[12];
                if (sc < nf && rc_isnum(f[sc]))
                    rc_fmt((float)atof(f[1 + 2 * i]), cell, sizeof(cell));
                else snprintf(cell, sizeof(cell), "%s", (sc < nf) ? f[sc] : "-");
                pdf_text_right(pd, M + timeW + colw * (i + 1) - 2,
                               ry + 10, fs, 0, 30, 30, 30, cell);
            }
            pdf_line(pd, M, ry + 13, M + tblw, ry + 13, 0.25f, 225);
            ry += 13; rowc++;
        }
        fclose(in);
    }

    /* ---------- signature / approval page (portrait) ---------- */
    pdf_page(pd, PW, PH);
    pdf_text(pd, M, M + 16, 14, 1, 13, 27, 42,
             "Report approval & electronic signature");
    float sy = M + 34;
    pdf_fill(pd, M, sy, PW - 2 * M, 20, 13, 27, 42);
    pdf_text(pd, M + 6, sy + 13, 9, 1, 255, 255, 255, "Action / Meaning");
    pdf_text(pd, M + 140, sy + 13, 9, 1, 255, 255, 255, "Printed name & role");
    pdf_text(pd, M + 320, sy + 13, 9, 1, 255, 255, 255, "Signature");
    pdf_text(pd, M + 440, sy + 13, 9, 1, 255, 255, 255, "Date & time");
    sy += 20;
    const char *acts[3] = { "Prepared / Author", "Reviewed", "Approved" };
    for (int i = 0; i < 3; i++) {
        pdf_rect(pd, M, sy, PW - 2 * M, 50, 0.6f, 150);
        pdf_line(pd, M + 130, sy, M + 130, sy + 50, 0.6f, 150);
        pdf_line(pd, M + 310, sy, M + 310, sy + 50, 0.6f, 150);
        pdf_line(pd, M + 430, sy, M + 430, sy + 50, 0.6f, 150);
        pdf_text(pd, M + 6, sy + 28, 9, 1, 13, 27, 42, acts[i]);
        sy += 50;
    }
    sy += 18;
    const char *stmt[] = {
      "21 CFR Part 11 Sec. 11.50 - Signature manifestations. Each signature above must record the",
      "signer's printed name, the date and time the signature was executed, and the meaning associated",
      "with the signature (authorship, review, or approval). This document is a controlled printout of",
      "electronic records stored on the recorder; the source records are attributable and time-stamped",
      "at acquisition. Any alteration of this printout voids its validity."
    };
    for (int i = 0; i < 5; i++) { pdf_text(pd, M, sy, 8.5f, 0, 70, 70, 70, stmt[i]); sy += 13; }
    { char idl[96]; snprintf(idl, sizeof(idl), "Report ID: %s     Generated: %s", rid, gen);
      pdf_text(pd, M, sy + 8, 8, 0, 150, 150, 150, idl); }

    /* ---------- save to USB ---------- */
    char dest[300];
    snprintf(dest, sizeof(dest),
             "%sreport_%04d%02d%02d-%02d%02d_%04d%02d%02d-%02d%02d.pdf",
             usb, a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min,
             b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, b.tm_hour, b.tm_min);
    int rc = pdf_save(pd, dest);
    pdf_free(pd);
    if (rc != 0) { snprintf(msg, msglen, "Cannot write PDF to USB"); return -1; }
    snprintf(msg, msglen, "Report saved to USB: %ld records, %d day(s)", total, nd);
    return (int)total;
}
