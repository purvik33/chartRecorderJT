/* export.h - time-range data export to a connected USB drive */
#ifndef EXPORT_H
#define EXPORT_H

#include <time.h>
#include <stddef.h>

/* find first removable/USB drive; returns 1 and fills path if found */
int usb_find(char *out, size_t n);

/* export samples between t0 and t1 (inclusive) to the USB drive.
 * Returns row count (>=0) on success, -1 on error.
 * msg receives a human-readable result either way. */
int export_range(time_t t0, time_t t1, char *msg, size_t msglen);

/* same, for the alarm event history (logs/alarms-*.csv) */
int export_alarms_range(time_t t0, time_t t1, char *msg, size_t msglen);

/* same, for the system event log (logs/events-*.csv) */
int export_events_range(time_t t0, time_t t1, char *msg, size_t msglen);

#endif
