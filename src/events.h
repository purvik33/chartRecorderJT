/* events.h - persistent system event log (audit trail).
 * One CSV per day: logs/events-YYYY-MM-DD.csv
 * Categories: SYSTEM, CONFIG, COMM, ALARM, CAL, EXPORT */
#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>
#include <time.h>

void event_log(const char *category, const char *fmt, ...);

/* one event-log row for on-screen viewing */
typedef struct {
    char ts[20];     /* "YYYY-MM-DD HH:MM:SS" */
    char cat[12];    /* category */
    char desc[80];   /* description */
    char user[20];   /* user (or SYSTEM) */
} event_rec_t;

/* Load event rows whose timestamp falls in [t0,t1] into out[], newest-first.
 * filter: 0 = all, 1 = exclude COMM (operator view), 2 = only COMM
 * (card-link diagnostics). Returns the count. */
int events_load(time_t t0, time_t t1, event_rec_t *out, int max, int filter);

/* verify the SHA-256 hash chain of a day's audit trail (date = "YYYY-MM-DD").
 * Returns 0 if intact, or the 1-based entry number where tampering is first
 * detected (-1 if the file is missing). `msg` gets a human-readable result. */
int event_audit_verify(const char *date, char *msg, size_t msglen);

#endif
