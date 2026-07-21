#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int history_load(time_t day_start, int group, trend_day_t *out)
{
    static float   sum[CH_PER_GROUP][TREND_BUCKETS];
    static int     cnt[CH_PER_GROUP][TREND_BUCKETS];
    memset(sum, 0, sizeof(sum));
    memset(cnt, 0, sizeof(cnt));
    memset(out, 0, sizeof(*out));

    struct tm dt = *localtime(&day_start);
    char path[64];
    snprintf(path, sizeof(path), "logs/%04d-%02d-%02d.csv",
             dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    int rows = 0;
    char line[4096];
    int first_field = group * CH_PER_GROUP * 2;   /* fields after timestamp */

    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "timestamp", 9)) continue;

        int Y, M, D, h, m, s;
        if (sscanf(line, "%4d-%2d-%2d %2d:%2d:%2d",
                   &Y, &M, &D, &h, &m, &s) != 6) continue;
        int bucket = (h * 3600 + m * 60 + s) / TREND_BUCKET_SEC;
        if (bucket < 0 || bucket >= TREND_BUCKETS) continue;
        rows++;

        /* walk comma-separated fields: value,status per channel */
        char *p = strchr(line, ',');
        int field = 0;
        float v = 0;
        while (p && field < first_field + CH_PER_GROUP * 2) {
            p++;
            if (field >= first_field) {
                int rel = field - first_field;
                int ch  = rel / 2;
                if ((rel & 1) == 0) {
                    v = strtof(p, NULL);
                } else {
                    /* status field: valid if OK / HI / LO */
                    if (!strncmp(p, "OK", 2) || !strncmp(p, "HI", 2) ||
                        !strncmp(p, "LO", 2)) {
                        sum[ch][bucket] += v;
                        cnt[ch][bucket]++;
                    }
                }
            }
            field++;
            p = strchr(p, ',');
        }
    }
    fclose(f);

    for (int c = 0; c < CH_PER_GROUP; c++)
        for (int b = 0; b < TREND_BUCKETS; b++)
            if (cnt[c][b] > 0) {
                out->val[c][b] = sum[c][b] / (float)cnt[c][b];
                out->ok[c][b]  = 1;
            }

    /* sample-hold: bridge short gaps (slow store intervals) so lines
     * stay continuous; gaps longer than 5 minutes remain real gaps */
    int max_hold = 300 / TREND_BUCKET_SEC;
    if (max_hold < 1) max_hold = 1;
    for (int c = 0; c < CH_PER_GROUP; c++) {
        int last = -1;
        for (int b = 0; b < TREND_BUCKETS; b++) {
            if (out->ok[c][b]) {
                last = b;
            } else if (last >= 0 && (b - last) <= max_hold) {
                out->val[c][b] = out->val[c][last];
                out->ok[c][b]  = 1;
            }
        }
    }
    return rows;
}

void history_overlay_live(trend_day_t *out, time_t day_start, int group)
{
    static float sum[CH_PER_GROUP][TREND_BUCKETS];
    static int   cnt[CH_PER_GROUP][TREND_BUCKETS];
    memset(sum, 0, sizeof(sum));
    memset(cnt, 0, sizeof(cnt));

    time_t now = time(NULL);
    data_lock();
    for (time_t t = now - LIVE_SECS + 1; t <= now; t++) {
        if (t < day_start || t >= day_start + 86400) continue;
        int b = (int)((t - day_start) / TREND_BUCKET_SEC);
        if (b < 0 || b >= TREND_BUCKETS) continue;
        for (int c = 0; c < CH_PER_GROUP; c++) {
            float v;
            if (data_live_get(group * CH_PER_GROUP + c, t, &v)) {
                sum[c][b] += v;
                cnt[c][b]++;
            }
        }
    }
    data_unlock();

    for (int c = 0; c < CH_PER_GROUP; c++)
        for (int b = 0; b < TREND_BUCKETS; b++)
            if (cnt[c][b] > 0) {
                out->val[c][b] = sum[c][b] / (float)cnt[c][b];
                out->ok[c][b]  = 1;
            }
}
