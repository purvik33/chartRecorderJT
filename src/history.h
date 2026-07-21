/* history.h - load stored samples back for the trend view.
 * A day is reduced to TREND_BUCKETS averaged points (2 min each). */
#ifndef HISTORY_H
#define HISTORY_H

#include "data_model.h"
#include <time.h>
#include <stdint.h>

#define TREND_BUCKET_SEC 30
#define TREND_BUCKETS    (86400 / TREND_BUCKET_SEC)   /* 2880 per day */

typedef struct {
    float   val[CH_PER_GROUP][TREND_BUCKETS];
    uint8_t ok[CH_PER_GROUP][TREND_BUCKETS];   /* 1 = valid measurement */
} trend_day_t;

/* day_start = local midnight of the requested day.
 * Returns number of data rows found (0 = nothing stored that day). */
int history_load(time_t day_start, int group, trend_day_t *out);

/* merge the last hour of once-per-second LIVE samples over the stored
 * buckets - live views update every second while the disk keeps only
 * the configured store interval */
void history_overlay_live(trend_day_t *out, time_t day_start, int group);

#endif
