#include "logger.h"
#include "config.h"
#include "data_model.h"
#include "events.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* Log retention is set by g_cfg.retention_days (0 = keep until the
 * disk-full safety net reclaims space). */

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
static void msleep(int ms) { Sleep(ms); }
static void make_dir(const char *p) { _mkdir(p); }
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <fcntl.h>
static void msleep(int ms) { usleep(ms * 1000); }
static void make_dir(const char *p) { mkdir(p, 0755); }
#endif

/* Force a file's buffered data all the way onto the storage medium, so a
 * power cut can lose at most the row being written now - never a previously
 * stored row and never the filesystem structure itself. */
static void file_durable(FILE *f)
{
    fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
}

/* fsync the directory too, so a freshly created day file's directory entry
 * survives a power cut (on ext4 a new file can otherwise vanish). */
static void dir_durable(const char *dir)
{
#ifdef _WIN32
    (void)dir;
#else
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { fsync(fd); close(fd); }
#endif
}

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

    file_durable(f);                 /* flush this row to the card before close */
    fclose(f);
    if (new_file) dir_durable("logs");   /* make the new file's dirent durable */
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

    int keep = g_cfg.retention_days;
    if (keep <= 0) { closedir(dp); return; }   /* 0 = keep until disk-full */

    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        int age = log_file_age_days(e->d_name, now);
        if (age > keep) {
            char path[300];
            snprintf(path, sizeof(path), "logs/%s", e->d_name);
            remove(path);
        }
    }
    closedir(dp);
}

/* ---- disk-full safety net --------------------------------------------------
 * Retention prunes by age; this prunes by FREE SPACE so the card can never
 * fill up and silently stop the recorder (or destabilise the OS). When free
 * space drops below the floor we delete WHOLE days oldest-first - the data
 * file plus its event and alarm files - and never touch today's file. */
#define DISK_WARN_FREE_MB 600
#define DISK_MIN_FREE_MB  250

static long disk_free_mb(void)
{
    struct statvfs v;
    if (statvfs("logs", &v) != 0) return -1;
    return (long)((double)v.f_bavail * (double)v.f_frsize / (1024.0 * 1024.0));
}

/* oldest logs/<date>.csv strictly before today_str; 0 if none */
static int oldest_log_date(const char *today_str, char out[11])
{
    DIR *dp = opendir("logs");
    if (!dp) return 0;
    char best[11] = "";
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        const char *n = e->d_name;
        if (strlen(n) != 14 || strcmp(n + 10, ".csv")) continue;  /* data file only */
        int y, m, d;
        if (sscanf(n, "%4d-%2d-%2d", &y, &m, &d) != 3) continue;
        char ds[11];
        memcpy(ds, n, 10); ds[10] = 0;
        if (strcmp(ds, today_str) >= 0) continue;                 /* never today */
        if (best[0] == 0 || strcmp(ds, best) < 0) memcpy(best, ds, 11);
    }
    closedir(dp);
    if (best[0] == 0) return 0;
    memcpy(out, best, 11);
    return 1;
}

static void disk_ensure_space(const char *today_str)
{
    long freemb = disk_free_mb();
    if (freemb < 0) return;                    /* statvfs failed - do nothing */

    static int warned = 0;
    if (freemb < DISK_WARN_FREE_MB && !warned) {
        warned = 1;
        event_log("SYSTEM", "Storage low: %ld MB free", freemb);
    } else if (freemb >= DISK_WARN_FREE_MB) {
        warned = 0;                            /* re-arm the warning */
    }

    int guard = 500;                           /* never spin forever */
    while (freemb >= 0 && freemb < DISK_MIN_FREE_MB && guard-- > 0) {
        char ds[11], p[300];
        if (!oldest_log_date(today_str, ds)) break;   /* nothing older to drop */
        snprintf(p, sizeof(p), "logs/%s.csv", ds);        remove(p);
        snprintf(p, sizeof(p), "logs/events-%s.csv", ds); remove(p);
        snprintf(p, sizeof(p), "logs/alarms-%s.csv", ds); remove(p);
        event_log("SYSTEM", "Storage full: deleted oldest logs for %s", ds);
        freemb = disk_free_mb();
    }
}
#else
static void logger_rotate(void) { }              /* no dirent on the Windows sim */
static void disk_ensure_space(const char *t) { (void)t; }
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
            char today_str[11];
            snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            disk_ensure_space(today_str);     /* free space before writing */
            write_sample(now - (sod % iv));   /* stamp the boundary */
        }
    }
    return NULL;
}

void logger_init(void)
{
    make_dir("logs");
    logger_rotate();               /* prune stale logs at boot */

    {   /* free-space safety check before the first sample is ever written */
        time_t now = time(NULL);
        struct tm tmv;
        loc_time(&now, &tmv);
        char today_str[11];
        snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
        disk_ensure_space(today_str);
    }

    pthread_t t;
    if (pthread_create(&t, NULL, logger_thread, NULL) != 0)
        event_log("SYSTEM", "logger thread failed to start");
}
