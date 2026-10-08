/* events.c - persistent system event log = 21 CFR Part 11 audit trail.
 * One CSV per day: logs/events-YYYY-MM-DD.csv, columns:
 *   timestamp,category,description,user,hash
 * The hash column is a SHA-256 chain: each entry hashes the previous
 * entry's hash together with this entry's fields, so any later edit,
 * insertion, deletion or reordering breaks the chain and is detected by
 * event_audit_verify(). The chain per day file is seeded from a fixed
 * genesis derived from the date. */
#include "events.h"
#include "users.h"
#include "crypto.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
static void ev_mkdir(void) { _mkdir("logs"); }
#else
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
static void ev_mkdir(void) { mkdir("logs", 0755); }
#endif

static pthread_mutex_t ev_mtx = PTHREAD_MUTEX_INITIALIZER;

static char ev_day[11]  = "";   /* day whose chain is currently loaded */
static char ev_prev[65] = "";   /* previous entry's hash (hex)         */

static void ev_genesis(const char *date, char out[65])
{
    char seed[40];
    snprintf(seed, sizeof(seed), "PR-40-AUDIT|%s", date);
    sha256_hex(seed, out);
}

/* load the previous hash for `date`: last data line's hash if the file
 * exists, else the day's genesis hash */
static void ev_load_prev(const char *path, const char *date)
{
    ev_genesis(date, ev_prev);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char l[512], last[512] = "";
    while (fgets(l, sizeof(l), f))
        if (strncmp(l, "timestamp", 9) && l[0] != '\n' && l[0] != '\r')
            snprintf(last, sizeof(last), "%s", l);
    fclose(f);
    if (!last[0]) return;
    char *h = strrchr(last, ',');
    if (!h) return;
    h++;
    size_t n = strlen(h);
    while (n && (h[n-1] == '\n' || h[n-1] == '\r' || h[n-1] == ' ')) h[--n] = 0;
    if (n == 64) { memcpy(ev_prev, h, 64); ev_prev[64] = 0; }
}

void event_log(const char *category, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    for (char *p = msg; *p; p++) if (*p == ',') *p = ';';

    time_t now = time(NULL);
    struct tm tm = *localtime(&now);
    char date[11], ts[20];
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(ts, sizeof(ts), "%s %02d:%02d:%02d",
             date, tm.tm_hour, tm.tm_min, tm.tm_sec);

    char path[64];
    snprintf(path, sizeof(path), "logs/events-%s.csv", date);

    const char *user = "SYSTEM";
    if (strcmp(category, "SYSTEM") != 0 && strcmp(category, "COMM") != 0)
        user = cfr_user_name();

    pthread_mutex_lock(&ev_mtx);
    ev_mkdir();

    FILE *chk = fopen(path, "r");
    int new_file = (chk == NULL);
    if (chk) fclose(chk);

    if (strcmp(ev_day, date) != 0) {
        ev_load_prev(path, date);
        snprintf(ev_day, sizeof(ev_day), "%s", date);
    }

    /* core = the four data fields; hash chains it to the previous hash */
    char core[300], hin[400], nh[65];
    snprintf(core, sizeof(core), "%s,%s,%s,%s", ts, category, msg, user);
    snprintf(hin, sizeof(hin), "%s|%s", ev_prev, core);
    sha256_hex(hin, nh);

    FILE *f = fopen(path, "a");
    if (f) {
        if (new_file)
            fprintf(f, "timestamp,category,description,user,hash\n");
        fprintf(f, "%s,%s\n", core, nh);
        /* flush the audit entry to the medium before returning: a power cut
         * must not silently drop the tail of the tamper-evident trail */
        fflush(f);
#ifdef _WIN32
        _commit(_fileno(f));
#else
        fsync(fileno(f));
        if (new_file) {
            int d = open("logs", O_RDONLY | O_DIRECTORY);
            if (d >= 0) { fsync(d); close(d); }
        }
#endif
        fclose(f);
        memcpy(ev_prev, nh, 65);
    }
    pthread_mutex_unlock(&ev_mtx);
}

int events_load(time_t t0, time_t t1, event_rec_t *out, int max, int filter)
{
    int n = 0;
    for (time_t day = t0; day <= t1 + 86400; day += 86400) {
        struct tm dt = *localtime(&day);
        char path[64];
        snprintf(path, sizeof(path), "logs/events-%04d-%02d-%02d.csv",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "timestamp", 9)) continue;
            char ts[20] = "", cat[12] = "", desc[80] = "",
                 user[20] = "", hash[72] = "";
            /* description is comma-sanitised by event_log, so this is safe */
            if (sscanf(line, "%19[^,],%11[^,],%79[^,],%19[^,],%71[^\r\n]",
                       ts, cat, desc, user, hash) < 4)
                continue;
            if (filter == 1 && !strcmp(cat, "COMM")) continue;
            if (filter == 2 &&  strcmp(cat, "COMM")) continue;

            struct tm tm = {0};
            if (sscanf(ts, "%4d-%2d-%2d %2d:%2d:%2d",
                       &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                       &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
                continue;
            tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
            time_t t = mktime(&tm);
            if (t < t0 || t > t1) continue;

            if (n == max) {                 /* keep the newest `max` */
                memmove(&out[0], &out[1], (size_t)(max - 1) * sizeof(out[0]));
                n--;
            }
            event_rec_t *r = &out[n++];
            snprintf(r->ts,   sizeof(r->ts),   "%s", ts);
            snprintf(r->cat,  sizeof(r->cat),  "%s", cat);
            snprintf(r->desc, sizeof(r->desc), "%s", desc);
            snprintf(r->user, sizeof(r->user), "%s", user);
        }
        fclose(f);
    }
    return n;   /* chronological; callers render newest-first */
}

/* recompute the chain for a day file; returns 0 if intact, else the
 * 1-based entry number where tampering is first detected. */
int event_audit_verify(const char *date, char *msg, size_t msglen)
{
    char path[64];
    snprintf(path, sizeof(path), "logs/events-%s.csv", date);
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(msg, msglen, "No event log for %s", date); return -1; }

    char prev[65];
    ev_genesis(date, prev);

    char line[512];
    int entry = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "timestamp", 9)) continue;
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        char *h = strrchr(line, ',');
        if (!h) { fclose(f); snprintf(msg, msglen, "Malformed entry %d", entry+1); return entry+1; }
        *h = 0;
        const char *stored = h + 1;

        char hin[400], calc[65];
        snprintf(hin, sizeof(hin), "%s|%s", prev, line);
        sha256_hex(hin, calc);
        entry++;
        if (strcmp(calc, stored) != 0) {
            fclose(f);
            snprintf(msg, msglen, "TAMPERING detected at entry %d of %s",
                     entry, date);
            return entry;
        }
        memcpy(prev, stored, 65);
    }
    fclose(f);
    snprintf(msg, msglen, "Audit trail intact - %d entries verified (%s)",
             entry, date);
    return 0;
}
