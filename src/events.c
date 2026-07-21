#include "events.h"
#include "users.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <direct.h>
static void ev_mkdir(void) { _mkdir("logs"); }
#else
#include <sys/stat.h>
static void ev_mkdir(void) { mkdir("logs", 0755); }
#endif

static pthread_mutex_t ev_mtx = PTHREAD_MUTEX_INITIALIZER;

void event_log(const char *category, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* commas would break the CSV columns - soften them */
    for (char *p = msg; *p; p++)
        if (*p == ',') *p = ';';

    time_t now = time(NULL);
    struct tm tm = *localtime(&now);

    char path[64];
    snprintf(path, sizeof(path), "logs/events-%04d-%02d-%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    pthread_mutex_lock(&ev_mtx);
    ev_mkdir();

    FILE *chk = fopen(path, "r");
    int new_file = (chk == NULL);
    if (chk) fclose(chk);

    /* user attribution (21 CFR): operator/config actions carry the
     * logged-in user; machine events stay SYSTEM */
    const char *user = "SYSTEM";
    if (strcmp(category, "SYSTEM") != 0 && strcmp(category, "COMM") != 0)
        user = cfr_user_name();

    FILE *f = fopen(path, "a");
    if (f) {
        if (new_file)
            fprintf(f, "timestamp,category,description,user\n");
        fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%s\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, category, msg, user);
        fclose(f);
    }
    pthread_mutex_unlock(&ev_mtx);
}
