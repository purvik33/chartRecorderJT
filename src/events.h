/* events.h - persistent system event log (audit trail).
 * One CSV per day: logs/events-YYYY-MM-DD.csv
 * Categories: SYSTEM, CONFIG, COMM, ALARM, CAL, EXPORT */
#ifndef EVENTS_H
#define EVENTS_H

void event_log(const char *category, const char *fmt, ...);

#endif
