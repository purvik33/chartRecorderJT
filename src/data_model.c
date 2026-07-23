#include "data_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

channel_t g_ch[CH_TOTAL];
static pthread_mutex_t data_mtx = PTHREAD_MUTEX_INITIALIZER;

void data_lock(void)   { pthread_mutex_lock(&data_mtx); }
void data_unlock(void) { pthread_mutex_unlock(&data_mtx); }

/* ---- live one-second ring ---- */
static float  lv_val[CH_TOTAL][LIVE_SECS];
static uint8_t lv_ok[CH_TOTAL][LIVE_SECS];
static time_t lv_t[LIVE_SECS];

void data_live_push(void)
{
    static time_t last_push;
    time_t now = time(NULL);

    pthread_mutex_lock(&data_mtx);
    /* back-fill seconds the acquisition cycle skipped so the live
     * ring is gapless (poll cycles are not exactly 1 Hz) */
    time_t from = now;
    if (last_push != 0 && last_push < now && now - last_push <= 10)
        from = last_push + 1;
    last_push = now;

    for (time_t t = from; t <= now; t++) {
        int s = (int)(t % LIVE_SECS);
        if (lv_t[s] == t) continue;
        lv_t[s] = t;
        for (int c = 0; c < CH_TOTAL; c++) {
            ch_status_t st = g_ch[c].status;
            if (st == CH_OK || st == CH_ALM_HI || st == CH_ALM_LO) {
                lv_val[c][s] = g_ch[c].value;
                lv_ok[c][s]  = 1;
            } else {
                lv_ok[c][s]  = 0;
            }
        }
    }
    pthread_mutex_unlock(&data_mtx);
}

int data_live_get(int ch, time_t t, float *v)
{
    if (ch < 0 || ch >= CH_TOTAL) return 0;
    int s = (int)(t % LIVE_SECS);
    /* read under the same lock the writer (data_live_push) holds so the
     * slot's timestamp/value/ok flags are observed consistently */
    data_lock();
    int ok = (lv_t[s] == t && lv_ok[ch][s]);
    if (ok) *v = lv_val[ch][s];
    data_unlock();
    return ok;
}

typedef struct {
    const char *prefix;
    const char *unit;
    const char *sensor;
    float lo, hi, alm_hi;
} ch_profile_t;

/* 8 profiles, one per channel position on every card */
static const ch_profile_t profiles[CH_PER_GROUP] = {
    { "TT", "\xC2\xB0""C", "Pt-100",  -200.0f,  850.0f,  700.0f },
    { "TT", "\xC2\xB0""C", "Pt-100",  -200.0f,  850.0f,  700.0f },
    { "PT", "bar",   "4-20 mA",    0.0f,   10.0f,    6.5f },
    { "FT", "m3/h",  "4-20 mA",    0.0f,  100.0f,   90.0f },
    { "TT", "\xC2\xB0""C", "Type K",  -200.0f, 1300.0f, 1100.0f },
    { "TT", "\xC2\xB0""C", "Type K",  -200.0f, 1300.0f, 1100.0f },
    { "LT", "%",     "0-5 V",      0.0f,  100.0f,   95.0f },
    { "RH", "%RH",   "4-20 mA",    0.0f,  100.0f,   85.0f },
};

static float phase[CH_TOTAL];
static uint32_t tick;

static float frand(void)
{
    return (float)rand() / (float)RAND_MAX;   /* 0..1 */
}

void data_model_init(void)
{
    for (int i = 0; i < CH_TOTAL; i++) {
        const ch_profile_t *p = &profiles[i % CH_PER_GROUP];
        channel_t *c = &g_ch[i];

        snprintf(c->tag, sizeof(c->tag), "%s-%d%02d",
                 p->prefix, (i / CH_PER_GROUP) + 1, (i % CH_PER_GROUP) + 1);
        strncpy(c->unit,   p->unit,   sizeof(c->unit) - 1);
        strncpy(c->sensor, p->sensor, sizeof(c->sensor) - 1);
        c->lo     = p->lo;
        c->hi     = p->hi;
        c->alm_hi = p->alm_hi;
        c->alm_lo = p->lo - 1.0f;   /* effectively disabled by default */
        c->div    = 10.0f;
        c->status = CH_OK;

        phase[i] = frand() * 6.28f;
        c->value = p->lo + (p->hi - p->lo) * (0.4f + 0.2f * frand());
    }
}

void data_sim_step(void)
{
    tick++;
    for (int i = 0; i < CH_TOTAL; i++) {
        channel_t *c = &g_ch[i];
        float mid  = c->lo + (c->hi - c->lo) * 0.5f;
        float amp  = (c->hi - c->lo) * 0.25f;
        float t    = (float)tick * 0.02f;

        c->value = mid
                 + amp * sinf(t + phase[i])
                 + (c->hi - c->lo) * 0.005f * (frand() - 0.5f);

        /* make CH3 of card 1 wander into high alarm now and then */
        if (i == 2) c->value = mid + amp * 1.6f * sinf(t * 0.35f);
    }
}
