#include "export.h"
#include "alarm.h"
#include <stdio.h>
#include <string.h>

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
