/* comm_sim.c - simulator data source: same thread shape as the Modbus
 * source, so the rest of the app cannot tell the difference. */
#include "comm.h"
#include "config.h"
#include "data_model.h"
#include "alarm.h"
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
static void msleep(int ms) { Sleep(ms); }
#else
#include <unistd.h>
static void msleep(int ms) { usleep(ms * 1000); }
#endif

int comm_modbus_link_ok(void);          /* from comm_modbus.c */
void *comm_modbus_thread(void *arg);    /* from comm_modbus.c */

static void *sim_thread(void *arg)
{
    (void)arg;
    while (1) {
        data_lock();
        data_sim_step();
        data_unlock();
        alarm_eval();
        data_live_push();
        msleep(500);
    }
    return NULL;
}

void comm_init(void)
{
    pthread_t t;
    if (g_cfg.source == SRC_MODBUS) {
        /* channels of cards that are not fitted: fixed to SKIP so no
         * stale/random values are ever shown or logged */
        data_lock();
        for (int i = g_cfg.cards * CH_PER_GROUP; i < CH_TOTAL; i++) {
            g_ch[i].value  = 0;
            g_ch[i].status = CH_SKIP;
        }
        data_unlock();
        pthread_create(&t, NULL, comm_modbus_thread, NULL);
    } else {
        pthread_create(&t, NULL, sim_thread, NULL);
    }
}

int comm_link_ok(void)
{
    if (g_cfg.source == SRC_MODBUS) return comm_modbus_link_ok();
    return 1;
}
