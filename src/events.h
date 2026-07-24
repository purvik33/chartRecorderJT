/* events.h - persistent system event log (audit trail).
 * One CSV per day: logs/events-YYYY-MM-DD.csv
 * Categories: SYSTEM, CONFIG, COMM, ALARM, CAL, EXPORT */
#ifndef EVENTS_H
#define EVENTS_H

#include <stddef.h>

void event_log(const char *category, const char *fmt, ...);

/* verify the SHA-256 hash chain of a day's audit trail (date = "YYYY-MM-DD").
 * Returns 0 if intact, or the 1-based entry number where tampering is first
 * detected (-1 if the file is missing). `msg` gets a human-readable result. */
int event_audit_verify(const char *date, char *msg, size_t msglen);

#endif
