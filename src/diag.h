/* diag.h - self diagnostics.
 * Runs automatically at every boot (background thread, results go to
 * the event log) and on demand from Menu > Factory settings. */
#ifndef DIAG_H
#define DIAG_H

#include <time.h>

typedef enum { DIAG_OK = 0, DIAG_WARN, DIAG_FAIL } diag_status_t;

typedef struct {
    char          name[28];
    diag_status_t status;
    char          detail[64];
} diag_item_t;

#define DIAG_MAX 16

/* run every test now (bounded: worst case a few seconds when cards
 * are absent); results are stored as the "last run" */
int  diag_run(void);

/* run in a background thread (for the UI: poll diag_busy(), then
 * read the results with diag_last() when it drops to 0) */
void diag_run_async(void);
int  diag_busy(void);

/* last stored results; returns item count, 0 = never ran */
int  diag_last(diag_item_t *out, time_t *when);

/* boot-time self test in a background thread + event log summary */
void diag_boot(void);

/* reboot the recorder hardware (Pi: system reboot; simulator: the
 * application relaunches itself). Logged before going down. */
void recorder_restart(void);

#endif
