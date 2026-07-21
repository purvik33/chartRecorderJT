/* data_model.h - latest-values table shared between the comm thread
 * (simulator or Modbus RTU poll of iAI_U8 cards) and the UI thread.
 * Always access g_ch under data_lock()/data_unlock() outside the UI
 * refresh path. */
#ifndef DATA_MODEL_H
#define DATA_MODEL_H

#include <stdint.h>

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
    ch_status_t status;
} channel_t;

extern channel_t g_ch[CH_TOTAL];

void data_model_init(void);
void data_lock(void);
void data_unlock(void);

void data_sim_step(void);   /* fake data generator (simulator source) */

/* ---- live ring: one sample per second for the last hour, in RAM.
 * Feeds the live trend/polar displays; storage on disk stays at the
 * configured store interval. */
#define LIVE_SECS 3600

void data_live_push(void);  /* called ~1 Hz by the acquisition thread */
/* value of channel ch at absolute second t; returns 1 if present */
int  data_live_get(int ch, time_t t, float *v);

#endif
