#include "logger.h"
#include "config.h"
#include "data_model.h"
#include "events.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* delete day-based log files older than this many days */
#define LOG_RETENTION_DAYS 90

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static void msleep(int ms) { Sleep(ms); }
static void make_dir(const char *p) { _mkdir(p); }
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
static void msleep(int ms) { usleep(ms * 1000); }
static void make_dir(const char *p) { mkdir(p, 0755); }
#endif

/* Portable localtime_r: the plain localtime() returns a pointer into a
 * shared static buffer, so a concurrent localtime() call in another
 * thread clobbers it. Always decode into a caller-owned struct tm. */
static struct tm *loc_time(const time_t *t, struct tm *out)
{
#ifdef _WIN32
    localtime_s(out, t);
    return out;
#else
    return localtime_r(t, out);
#endif
}

static const char *status_txt(ch_status_t s)
{
    switch (s) {
    case CH_ALM_HI: return "HI";
    case CH_ALM_LO: return "LO";
    case CH_SKIP:   return "SKIP";
    case CH_UNDER:  return "UNDER";
    case CH_OVER:   return "OVER";
    case CH_OPEN:   return "OPEN";
    case CH_COMM:   return "COMM";
    default:        return "OK";
    }
}

static void write_sample(time_t when)
{
    struct tm tmv;
    struct tm *tm = loc_time(&when, &tmv);

    char path[64];
    snprintf(path, sizeof(path), "logs/%04d-%02d-%02d.csv",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

    FILE *f = fopen(path, "r");
    int new_file = (f == NULL);
    if (f) fclose(f);

    f = fopen(path, "a");
    if (!f) return;

    float  val[CH_TOTAL];
    ch_status_t st[CH_TOTAL];
    char   tags[CH_TOTAL][12];

    data_lock();
    for (int i = 0; i < CH_TOTAL; i++) {
        val[i] = g_ch[i].value;
        st[i]  = g_ch[i].status;
        memcpy(tags[i], g_ch[i].tag, sizeof(tags[i]));
    }
    data_unlock();

    if (new_file) {
        fprintf(f, "timestamp");
        for (int i = 0; i < CH_TOTAL; i++)
            fprintf(f, ",%s,%s_st", tags[i], tags[i]);
        fprintf(f, "\n");
    }

    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    for (int i = 0; i < CH_TOTAL; i++)
        fprintf(f, ",%.3f,%s", (double)val[i], status_txt(st[i]));
    fprintf(f, "\n");
    fclose(f);
}

/* Retention: remove day-based log files (logs/YYYY-MM-DD.csv,
 * logs/events-YYYY-MM-DD.csv, logs/alarms-YYYY-MM-DD.csv) older than
 * LOG_RETENTION_DAYS. Only names matching the dated pattern exactly are
 * touched - anything else in logs/ is left strictly alone. */
#ifndef _WIN32
static int log_file_age_days(const char *name, time_t now)
{
    const char *d = name;
    if (!strncmp(name, "events-", 7) || !strncmp(name, "alarms-", 7))
        d = name + 7;

    /* must be exactly "YYYY-MM-DD.csv" and nothing more */
    if (strlen(d) != 14 || strcmp(d + 10, ".csv")) return -1;

    int y, m, day;
    if (sscanf(d, "%4d-%2d-%2d", &y, &m, &day) != 3) return -1;
    if (m < 1 || m > 12 || day < 1 || day > 31) return -1;

    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year  = y - 1900;
    tmv.tm_mon   = m - 1;
    tmv.tm_mday  = day;
    tmv.tm_hour  = 12;          /* midday: keeps DST off the edges */
    tmv.tm_isdst = -1;
    time_t ft = mktime(&tmv);
    if (ft == (time_t)-1) return -1;

    return (int)((now - ft) / 86400);
}

static void logger_rotate(void)
{
    DIR *dp = opendir("logs");
    if (!dp) return;

    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        int age = log_file_age_days(e->d_name, now);
        if (age > LOG_RETENTION_DAYS) {
            char path[300];
            snprintf(path, sizeof(path), "logs/%s", e->d_name);
            remove(path);
        }
    }
    closedir(dp);
}
#else
static void logger_rotate(void) { }   /* no dirent on the Windows sim */
#endif

/* Samples are stored on wall-clock boundaries, not "N seconds after
 * start": a 1-minute interval stores at 10:01:00, 10:02:00, ...; a
 * 5-minute interval at 10:05:00, 10:10:00, ... The stored timestamp
 * is the boundary itself, so the CSV rows line up exactly. */
static void *logger_thread(void *arg)
{
    (void)arg;
    long last_slot = -1;
    long last_rot_day = -1;
    while (1) {
        msleep(1000);

        int iv = g_cfg.store_interval;
        if (iv < 60) iv = 60;          /* minimum interval: 1 minute */

        time_t now = time(NULL);
        struct tm tm;
        loc_time(&now, &tm);
        int  sod  = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        long slot = sod / iv;

        /* prune expired logs once per calendar day */
        long day = (long)tm.tm_year * 366 + tm.tm_yday;
        if (day != last_rot_day) {
            last_rot_day = day;
            logger_rotate();
        }

        if (last_slot < 0) {           /* boot: wait for a boundary */
            last_slot = slot;
            continue;
        }
        if (slot != last_slot) {
            last_slot = slot;
            write_sample(now - (sod % iv));   /* stamp the boundary */
        }
    }
    return NULL;
}

void logger_init(void)
{
    make_dir("logs");
    logger_rotate();               /* prune stale logs at boot */
    pthread_t t;
    if (pthread_create(&t, NULL, logger_thread, NULL) != 0)
        event_log("SYSTEM", "logger thread failed to start");
}
