#include "logger.h"
#include "config.h"
#include "data_model.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static void msleep(int ms) { Sleep(ms); }
static void make_dir(const char *p) { _mkdir(p); }
#else
#include <unistd.h>
#include <sys/stat.h>
static void msleep(int ms) { usleep(ms * 1000); }
static void make_dir(const char *p) { mkdir(p, 0755); }
#endif

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
    struct tm *tm = localtime(&when);

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

/* Samples are stored on wall-clock boundaries, not "N seconds after
 * start": a 1-minute interval stores at 10:01:00, 10:02:00, ...; a
 * 5-minute interval at 10:05:00, 10:10:00, ... The stored timestamp
 * is the boundary itself, so the CSV rows line up exactly. */
static void *logger_thread(void *arg)
{
    (void)arg;
    long last_slot = -1;
    while (1) {
        msleep(1000);

        int iv = g_cfg.store_interval;
        if (iv < 60) iv = 60;          /* minimum interval: 1 minute */

        time_t now = time(NULL);
        struct tm tm = *localtime(&now);
        int  sod  = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        long slot = sod / iv;

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
    pthread_t t;
    pthread_create(&t, NULL, logger_thread, NULL);
}
