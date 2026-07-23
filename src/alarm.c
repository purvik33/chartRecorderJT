#include "alarm.h"
#include "data_model.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- persistent alarm event log: logs/alarms-YYYY-MM-DD.csv ----
 * One row per event action (SET / CLEAR / ACK), chronological, so the
 * full alarm history survives restarts and can be exported. */
static const char *type_txt_log[] = { "HIGH", "LOW", "COMM", "OPEN" };

/* a formatted-but-not-yet-written log row: capture the target file and
 * the exact CSV line while under the data lock, flush to disk later */
typedef struct {
    char path[64];
    char line[200];
} alarm_row_t;

/* format the row (reads g_ch/e - must run while holding the data lock) */
static void alarm_fmt_row(alarm_row_t *row, const alarm_evt_t *e,
                          const char *action)
{
    time_t now = time(NULL);
    struct tm tm = *localtime(&now);

    snprintf(row->path, sizeof(row->path), "logs/alarms-%04d-%02d-%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(row->line, sizeof(row->line),
             "%04d-%02d-%02d %02d:%02d:%02d,CH%d,%s,%s,%s,%.3f\n",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             e->ch + 1, g_ch[e->ch].tag,
             type_txt_log[e->type], action, (double)e->value);
}

/* write a pre-formatted row to disk (no data lock needed / must not hold
 * it - blocking SD-card I/O) */
static void alarm_write_row(const alarm_row_t *row)
{
    FILE *chk = fopen(row->path, "r");
    int new_file = (chk == NULL);
    if (chk) fclose(chk);

    FILE *f = fopen(row->path, "a");
    if (!f) return;
    if (new_file)
        fprintf(f, "timestamp,channel,tag,type,event,value\n");
    fputs(row->line, f);
    fclose(f);
}

static void alarm_log_row(const alarm_evt_t *e, const char *action)
{
    alarm_row_t row;
    alarm_fmt_row(&row, e, action);
    alarm_write_row(&row);
}

/* rows produced during one locked alarm_eval() pass, flushed after unlock.
 * Worst case is a close + a re-open per channel. */
static alarm_row_t pend_rows[2 * CH_TOTAL];
static int         pend_n;

static void alarm_stage_row(const alarm_evt_t *e, const char *action)
{
    if (pend_n < (int)(sizeof(pend_rows) / sizeof(pend_rows[0])))
        alarm_fmt_row(&pend_rows[pend_n++], e, action);
}

/* ring buffer of events, newest at (head-1) */
static alarm_evt_t hist[ALARM_HIST];
static int head, count;

/* per-channel latched state: index into hist of the open event, or -1 */
static int open_evt[CH_TOTAL];
static int inited;

static void push_event(int ch, alarm_type_t type, float value)
{
    alarm_evt_t *e = &hist[head];
    /* if we overwrite an event that is still open, drop its reference */
    for (int i = 0; i < CH_TOTAL; i++)
        if (open_evt[i] == head) open_evt[i] = -1;

    e->t_set   = time(NULL);
    e->t_clear = 0;
    e->t_ack   = 0;
    e->ch      = ch;
    e->type    = type;
    e->value   = value;
    e->acked   = 0;
    e->deleted = 0;
    open_evt[ch] = head;

    head = (head + 1) % ALARM_HIST;
    if (count < ALARM_HIST) count++;

    alarm_stage_row(e, "SET");
}

static void close_event(int ch)
{
    if (open_evt[ch] >= 0) {
        alarm_evt_t *e = &hist[open_evt[ch]];
        e->t_clear = time(NULL);
        if (e->acked) e->deleted = 1;   /* acked + cleared: drop it */
        open_evt[ch] = -1;
        alarm_stage_row(e, "CLEAR");
    }
}

void alarm_eval(void)
{
    data_lock();
    pend_n = 0;
    if (!inited) {
        for (int i = 0; i < CH_TOTAL; i++) open_evt[i] = -1;
        inited = 1;
    }

    for (int i = 0; i < CH_TOTAL; i++) {
        channel_t *c = &g_ch[i];
        if (c->status == CH_COMM || c->status == CH_OPEN) {
            alarm_type_t t = (c->status == CH_OPEN) ? ALM_OPEN : ALM_COMM;
            if (open_evt[i] < 0 || hist[open_evt[i]].type != t) {
                close_event(i);
                push_event(i, t, 0);
            }
            continue;
        }
        if (c->status == CH_SKIP || c->status == CH_UNDER ||
            c->status == CH_OVER) {
            /* not valid measurements - no hi/lo evaluation, no event */
            close_event(i);
            continue;
        }

        /* channel is measuring again: close any open fault event
         * (sensor reconnected / card responding again) */
        if (open_evt[i] >= 0 &&
            (hist[open_evt[i]].type == ALM_COMM ||
             hist[open_evt[i]].type == ALM_OPEN))
            close_event(i);

        float hys = (c->hi - c->lo) * 0.005f;   /* 0.5 % hysteresis */
        ch_status_t ns = CH_OK;

        if      (c->value > c->alm_hi)       ns = CH_ALM_HI;
        else if (c->value < c->alm_lo)       ns = CH_ALM_LO;
        else if (c->status == CH_ALM_HI && c->value > c->alm_hi - hys) ns = CH_ALM_HI;
        else if (c->status == CH_ALM_LO && c->value < c->alm_lo + hys) ns = CH_ALM_LO;

        if (ns != c->status) {
            if (ns == CH_ALM_HI)      push_event(i, ALM_HI, c->value);
            else if (ns == CH_ALM_LO) push_event(i, ALM_LO, c->value);
            else                      close_event(i);
            c->status = ns;
        }
    }
    data_unlock();

    /* flush the captured rows to disk now the data lock is released, so
     * blocking SD-card I/O never stalls the acquisition thread / UI */
    for (int i = 0; i < pend_n; i++)
        alarm_write_row(&pend_rows[i]);
}

int alarm_active_count(void)
{
    data_lock();
    int n = 0;
    for (int i = 0; i < CH_TOTAL; i++)
        if (open_evt[i] >= 0) n++;
    data_unlock();
    return n;
}

int alarm_unacked_count(void)
{
    data_lock();
    int n = 0;
    for (int i = 0; i < count; i++)
        if (!hist[i].acked && !hist[i].deleted) n++;
    data_unlock();
    return n;
}

void alarm_ack_all(void)
{
    int nacked = 0;
    data_lock();
    for (int i = 0; i < count; i++) {
        if (!hist[i].acked && !hist[i].deleted) {
            hist[i].t_ack = time(NULL);
            alarm_log_row(&hist[i], "ACK");
            nacked++;
        }
        hist[i].acked = 1;
        if (hist[i].t_clear != 0) hist[i].deleted = 1;
    }
    data_unlock();
    if (nacked > 0)
        event_log("ALARM", "Acknowledged %d alarm(s)", nacked);
}

int alarm_records_load(time_t t0, time_t t1, alarm_rec_t *out, int max)
{
    int n = 0;

    for (time_t day = t0 - 86400; day <= t1 + 86400; day += 86400) {
        struct tm dt = *localtime(&day);
        char path[64];
        snprintf(path, sizeof(path), "logs/alarms-%04d-%02d-%02d.csv",
                 dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[220];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "timestamp", 9)) continue;

            char ts[24] = "", ch[8] = "", tag[16] = "", ty[8] = "",
                 ev[8] = "", vv[20] = "";
            if (sscanf(line,
                       "%23[^,],%7[^,],%15[^,],%7[^,],%7[^,],%19[^\r\n]",
                       ts, ch, tag, ty, ev, vv) < 5)
                continue;

            struct tm tm = {0};
            if (sscanf(ts, "%4d-%2d-%2d %2d:%2d:%2d",
                       &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                       &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
                continue;
            tm.tm_year -= 1900;
            tm.tm_mon  -= 1;
            tm.tm_isdst = -1;
            time_t t = mktime(&tm);

            if (!strcmp(ev, "SET")) {
                if (t < t0 || t > t1) continue;
                if (n == max) {          /* keep the newest */
                    memmove(&out[0], &out[1],
                            (size_t)(max - 1) * sizeof(out[0]));
                    n--;
                }
                alarm_rec_t *r = &out[n++];
                snprintf(r->ch,  sizeof(r->ch),  "%s", ch);
                snprintf(r->tag, sizeof(r->tag), "%s", tag);
                snprintf(r->type, sizeof(r->type), "%s", ty);
                r->value = strtof(vv, NULL);
                snprintf(r->set_ts, sizeof(r->set_ts), "%s", ts);
                r->ack_ts[0] = 0;
                r->clr_ts[0] = 0;
            } else {
                /* attach ACK / CLEAR to the latest matching open record */
                for (int i = n - 1; i >= 0; i--) {
                    if (strcmp(out[i].ch, ch) || strcmp(out[i].type, ty))
                        continue;
                    if (!strcmp(ev, "ACK") && out[i].ack_ts[0] == 0) {
                        snprintf(out[i].ack_ts, sizeof(out[i].ack_ts),
                                 "%s", ts);
                        break;
                    }
                    if (!strcmp(ev, "CLEAR") && out[i].clr_ts[0] == 0) {
                        snprintf(out[i].clr_ts, sizeof(out[i].clr_ts),
                                 "%s", ts);
                        break;
                    }
                    if (!strcmp(ev, "ACK") || !strcmp(ev, "CLEAR"))
                        break;
                }
            }
        }
        fclose(f);
    }
    return n;
}

int alarm_get_history(alarm_evt_t *out, int max)
{
    data_lock();
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        int idx = (head - 1 - i + ALARM_HIST) % ALARM_HIST;
        if (hist[idx].deleted) continue;
        out[n++] = hist[idx];
    }
    data_unlock();
    return n;
}
