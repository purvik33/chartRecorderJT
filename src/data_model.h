/* data_model.h - latest-values table shared between the comm thread
 * (simulator or Modbus RTU poll of iAI_U8 cards) and the UI thread.
 * Always access g_ch under data_lock()/data_unlock() outside the UI
 * refresh path. */
#ifndef DATA_MODEL_H
#define DATA_MODEL_H

#include <stdint.h>
#include <time.h>

#define CH_TOTAL      40   /* 5 cards x 8 channels */
#define CH_PER_GROUP  8
#define GROUP_COUNT   (CH_TOTAL / CH_PER_GROUP)

/* iAI_U8 abnormal value codes: 32764 skip, 32765 under, 32766 over,
 * 32767 open */
typedef enum {
    CH_OK = 0,
    CH_ALM_HI,
    CH_ALM_LO,
    CH_SKIP,     /* channel skipped / disabled on the card (32764) */
    CH_UNDER,    /* under range (32765) */
    CH_OVER,     /* over range (32766) */
    CH_OPEN,     /* sensor open / burnout (32767) */
    CH_COMM      /* card not responding on the bus */
} ch_status_t;

typedef struct {
    char        tag[12];     /* e.g. "TT-101" */
    char        unit[8];     /* e.g. "degC"   */
    char        sensor[12];  /* e.g. "Pt-100" */
    float       lo;          /* range low  */
    float       hi;          /* range high */
    float       alm_hi;      /* high alarm setpoint */
    float       alm_lo;      /* low alarm setpoint  */
    float       value;       /* latest engineering value */
    float       div;         /* raw-to-engineering divisor (type based) */
    int         lin;         /* 1 = linear input: scale ADC counts to [lo,hi] */
    float       cnt_lo;      /* ADC count that maps to range low  (user zero) */
    float       cnt_hi;      /* ADC count that maps to range high (user span) */
    int         decimals;    /* display decimal places (linear inputs only) */
    ch_status_t status;
} channel_t;

extern channel_t g_ch[CH_TOTAL];

void data_model_init(void);
void data_lock(void);
void data_unlock(void);

/* Round v to `dec` decimals for display and fold negative zero into +0, so a
 * reading like -0.03 shown at 1 decimal appears as "0.0", never "-0.0". */
static inline double disp_fix(double v, int dec)
{
    double p = 1.0;
    for (int i = 0; i < dec; i++) p *= 10.0;
    double r = (v < 0.0 ? -(double)(long long)(-v * p + 0.5)
                        :  (double)(long long)( v * p + 0.5)) / p;
    return r == 0.0 ? 0.0 : r;   /* assigning 0.0 yields +0.0 */
}

/* Display decimal places for a channel: linear inputs use the per-channel
 * setting; RTD/TC keep the card's fixed 1-decimal presentation. */
static inline int ch_dec(const channel_t *c)
{
    return c->lin ? (c->decimals < 0 ? 0 : c->decimals > 4 ? 4 : c->decimals) : 1;
}

/* Format v into buf with `dec` decimals, folding -0 into 0. Returns buf. */
const char *disp_str(char *buf, int n, double v, int dec);

void data_sim_step(void);   /* fake data generator (simulator source) */

/* ---- live ring: one sample per second for the last hour, in RAM.
 * Feeds the live trend/polar displays; storage on disk stays at the
 * configured store interval. */
#define LIVE_SECS 3600

void data_live_push(void);  /* called ~1 Hz by the acquisition thread */
/* value of channel ch at absolute second t; returns 1 if present */
int  data_live_get(int ch, time_t t, float *v);

#endif
