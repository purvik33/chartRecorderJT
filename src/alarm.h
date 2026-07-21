/* alarm.h - alarm evaluation, active list and event history */
#ifndef ALARM_H
#define ALARM_H

#include <time.h>
#include <stdint.h>

#define ALARM_HIST 100

typedef enum { ALM_HI = 0, ALM_LO, ALM_COMM, ALM_OPEN } alarm_type_t;

typedef struct {
    time_t       t_set;
    time_t       t_clear;    /* 0 while active */
    time_t       t_ack;      /* 0 until acknowledged */
    int          ch;         /* 0-based channel */
    alarm_type_t type;
    float        value;      /* value at trigger */
    uint8_t      acked;
    uint8_t      deleted;    /* acked AND cleared: drop from the list */
} alarm_evt_t;

/* one alarm with its full lifecycle, reconstructed from the event log */
typedef struct {
    char  ch[8];
    char  tag[16];
    char  type[8];
    float value;
    char  set_ts[20];        /* "YYYY-MM-DD HH:MM:SS" */
    char  ack_ts[20];        /* "" if never acknowledged */
    char  clr_ts[20];        /* "" if still active */
} alarm_rec_t;

/* load alarm records whose SET time falls in [t0, t1] from the
 * persistent event log; returns count (chronological order) */
int alarm_records_load(time_t t0, time_t t1, alarm_rec_t *out, int max);

/* called by the comm thread after each data update (takes data lock) */
void alarm_eval(void);

/* UI-side accessors (take the lock internally) */
int  alarm_active_count(void);
int  alarm_unacked_count(void);
void alarm_ack_all(void);
/* copy newest-first into out[], returns number copied */
int  alarm_get_history(alarm_evt_t *out, int max);

#endif
