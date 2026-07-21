/* diag.c - self diagnostics: storage, clock, memory, RS-485 bus and
 * every fitted AI card, Modbus TCP, network. All tests are bounded
 * (serial timeouts), so a run can never hang. */
#include "diag.h"
#include "config.h"
#include "comm.h"
#include "events.h"
#include "modbus_tcp.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

static diag_item_t     last_items[DIAG_MAX];
static int             last_count;
static time_t          last_when;
static pthread_mutex_t diag_mtx = PTHREAD_MUTEX_INITIALIZER;

static diag_item_t *add(diag_item_t *it, int *n, const char *name,
                        diag_status_t st, const char *fmt, ...)
{
    if (*n >= DIAG_MAX) return NULL;
    diag_item_t *d = &it[(*n)++];
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->status = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->detail, sizeof(d->detail), fmt, ap);
    va_end(ap);
    return d;
}

static void free_space_gb(double *free_gb, double *total_gb)
{
    *free_gb = *total_gb = 0;
#ifdef _WIN32
    ULARGE_INTEGER avail, total;
    if (GetDiskFreeSpaceExA(".", &avail, &total, NULL)) {
        *free_gb  = (double)avail.QuadPart / 1e9;
        *total_gb = (double)total.QuadPart / 1e9;
    }
#else
    struct statvfs v;
    if (statvfs(".", &v) == 0) {
        *free_gb  = (double)v.f_bavail * (double)v.f_frsize / 1e9;
        *total_gb = (double)v.f_blocks * (double)v.f_frsize / 1e9;
    }
#endif
}

int diag_run(void)
{
    diag_item_t it[DIAG_MAX];
    int n = 0;

    /* configuration file readable + writable */
    {
        FILE *f = fopen("recorder.ini", "r+");
        if (f) { fclose(f); add(it, &n, "Configuration", DIAG_OK,
                                "recorder.ini read/write OK"); }
        else     add(it, &n, "Configuration", DIAG_FAIL,
                     "recorder.ini not writable");
    }

    /* data storage: write + delete a probe file, free space */
    {
        double fg, tg;
        free_space_gb(&fg, &tg);
        FILE *f = fopen("logs/.diagtest", "w");
        int wr = 0;
        if (f) { wr = fputs("test", f) >= 0; fclose(f);
                 remove("logs/.diagtest"); }
        if (!f || !wr)
            add(it, &n, "Data storage", DIAG_FAIL, "logs/ not writable");
        else if (fg < 0.5)
            add(it, &n, "Data storage", DIAG_WARN,
                "only %.1f GB free of %.0f GB", fg, tg);
        else
            add(it, &n, "Data storage", DIAG_OK,
                "writable, %.1f GB free of %.0f GB", fg, tg);
    }

    /* system clock plausibility */
    {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        if (tm && tm->tm_year + 1900 >= 2024 && tm->tm_year + 1900 <= 2099)
            add(it, &n, "System clock", DIAG_OK, "%04d-%02d-%02d %02d:%02d",
                tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                tm->tm_hour, tm->tm_min);
        else
            add(it, &n, "System clock", DIAG_FAIL,
                "date invalid - set the clock");
    }

    /* memory allocator sanity */
    {
        void *p = malloc(1 << 20);
        if (p) { memset(p, 0xA5, 1 << 20); free(p);
                 add(it, &n, "Memory", DIAG_OK, "allocator OK"); }
        else     add(it, &n, "Memory", DIAG_FAIL, "1 MB allocation failed");
    }

    /* RS-485 bus + every fitted card */
    if (g_cfg.source != SRC_MODBUS) {
        add(it, &n, "RS-485 bus", DIAG_WARN, "demo mode - bus not in use");
    } else {
        add(it, &n, "RS-485 bus", comm_link_ok() ? DIAG_OK : DIAG_FAIL,
            comm_link_ok() ? "%s @ %d baud, link up"
                           : "%s @ %d baud, no link",
            g_cfg.port, g_cfg.baud);
        for (int card = 0; card < g_cfg.cards; card++) {
            char nm[28];
            snprintf(nm, sizeof(nm), "AI card %d", card + 1);
            uint16_t t[1];
            if (mb_service_read(g_cfg.slave_base + card, 3, REG_TYPE,
                                1, t) == 0)
                add(it, &n, nm, DIAG_OK, "slave %d responding",
                    g_cfg.slave_base + card);
            else
                add(it, &n, nm, DIAG_FAIL, "slave %d no reply",
                    g_cfg.slave_base + card);
        }
    }

    /* Modbus TCP server */
    if (g_cfg.tcp_enable)
        add(it, &n, "Modbus TCP", DIAG_OK, "port %d, unit %d, %d client(s)",
            g_cfg.tcp_port, g_cfg.tcp_unit, modbus_tcp_clients());
    else
        add(it, &n, "Modbus TCP", DIAG_WARN, "server disabled");

    /* network address */
    {
        char ip[24];
        net_current_ip(ip, sizeof(ip));
        if (strcmp(ip, "-") != 0)
            add(it, &n, "Network", DIAG_OK, "address %s", ip);
        else
            add(it, &n, "Network", DIAG_WARN, "no IP address");
    }

    pthread_mutex_lock(&diag_mtx);
    memcpy(last_items, it, sizeof(it[0]) * (size_t)n);
    last_count = n;
    last_when  = time(NULL);
    pthread_mutex_unlock(&diag_mtx);

    int warn = 0, fail = 0;
    for (int i = 0; i < n; i++) {
        if (it[i].status == DIAG_WARN) warn++;
        if (it[i].status == DIAG_FAIL) fail++;
    }
    event_log("SYSTEM", "Self test: %d checks, %d warning(s), %d"
              " failure(s)", n, warn, fail);
    for (int i = 0; i < n; i++)
        if (it[i].status == DIAG_FAIL)
            event_log("SYSTEM", "Self test FAIL: %s - %s",
                      it[i].name, it[i].detail);
    return n;
}

int diag_last(diag_item_t *out, time_t *when)
{
    pthread_mutex_lock(&diag_mtx);
    int n = last_count;
    memcpy(out, last_items, sizeof(last_items[0]) * (size_t)n);
    if (when) *when = last_when;
    pthread_mutex_unlock(&diag_mtx);
    return n;
}

static volatile int busy_f;
int diag_busy(void) { return busy_f; }

static void *diag_async_thread(void *arg)
{
    (void)arg;
    diag_run();
    busy_f = 0;
    return NULL;
}

void diag_run_async(void)
{
    if (busy_f) return;
    busy_f = 1;
    pthread_t t;
    pthread_create(&t, NULL, diag_async_thread, NULL);
}

static void *diag_boot_thread(void *arg)
{
    (void)arg;
    /* give the comm thread time to bring the bus up first */
#ifdef _WIN32
    Sleep(3000);
#else
    sleep(3);
#endif
    diag_run();
    return NULL;
}

void diag_boot(void)
{
    pthread_t t;
    pthread_create(&t, NULL, diag_boot_thread, NULL);
}

void recorder_restart(void)
{
    event_log("SYSTEM", "Recorder restart requested from menu");
#ifdef _WIN32
    /* simulator: relaunch the exe so the operator sees the same
     * splash-to-running cycle the real hardware performs */
    char p[MAX_PATH];
    if (GetModuleFileNameA(NULL, p, MAX_PATH)) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessA(p, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                           &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    exit(0);
#else
    /* real hardware: full OS reboot (the app runs as a root systemd
     * service on the product, so `reboot` is available) */
    if (system("reboot &") != 0) {
        if (system("sudo reboot &") != 0) { /* fall through to exit */ }
    }
    exit(0);   /* systemd Restart=always brings the app back if the
                * reboot command was unavailable */
#endif
}
