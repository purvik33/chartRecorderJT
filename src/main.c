/* main.c - entry point.
 * RECORDER_SIM: opens an 800x480 SDL window on the PC (mouse = touch).
 * RECORDER_PI : renders to the DSI panel via DRM, touch via evdev. */
#include "lvgl.h"
#include "data_model.h"
#include "config.h"
#include "comm.h"
#include "logger.h"
#include "events.h"
#include "modbus_tcp.h"
#include "webserver.h"
#include "diag.h"
#include "ui/ui.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
static void sleep_ms(uint32_t ms) { Sleep(ms); }
/* run from the exe's own folder so recorder.ini and logs/ are always
 * found there, regardless of how the app was launched */
static void chdir_to_exe(void)
{
    char p[MAX_PATH];
    if (GetModuleFileNameA(NULL, p, MAX_PATH)) {
        char *s = strrchr(p, '\\');
        if (s) { *s = 0; _chdir(p); }
    }
}
#else
#include <unistd.h>
static void sleep_ms(uint32_t ms) { usleep(ms * 1000); }
static void chdir_to_exe(void)
{
    char p[512];
    ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (n > 0) {
        p[n] = 0;
        char *s = strrchr(p, '/');
        if (s) { *s = 0; if (chdir(p) != 0) { /* keep cwd */ } }
    }
}
#endif

static uint32_t tick_cb(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static void recorder_stop_log(void)
{
    event_log("SYSTEM", "Recorder stopped");
}

int main(int argc, char **argv)
{
    chdir_to_exe();
    srand((unsigned)time(NULL));

    data_model_init();
    config_load();     /* recorder.ini: comm, channels, brand */

    lv_init();
    lv_tick_set_cb(tick_cb);

#ifdef RECORDER_SIM
    LV_UNUSED(argc); LV_UNUSED(argv);
    lv_display_t *disp = lv_sdl_window_create(800, 480);
    char title[80];
    snprintf(title, sizeof(title), "%s  %s", g_cfg.brand, g_cfg.model);
    lv_sdl_window_set_title(disp, title);
    lv_sdl_mouse_create();
#else
    /* Usage: recorder_ui [drm-card] [input-event]
     * HDMI + mouse:   ./recorder_ui /dev/dri/card1 /dev/input/event0
     * DSI + touch:    ./recorder_ui /dev/dri/card0 /dev/input/event4
     * Defaults below; find devices with `ls /dev/dri` and `evtest`. */
    const char *drm_path = (argc > 1) ? argv[1] : "/dev/dri/card0";
    const char *inp_path = (argc > 2) ? argv[2] : "/dev/input/event0";

    lv_display_t *disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, drm_path, -1);
    lv_evdev_create(LV_INDEV_TYPE_POINTER, inp_path);
#endif

    event_log("SYSTEM", "Recorder started (%s)",
              g_cfg.source == SRC_MODBUS ? "input cards" : "demo mode");
    atexit(recorder_stop_log);

    ui_init();
    comm_init();       /* demo data or Modbus RTU thread per config */
    logger_init();     /* data storage thread */
    modbus_tcp_init(); /* Modbus TCP server + RS-485 gateway thread */
    webserver_init();  /* read-only dashboard + CSV download        */
    diag_boot();       /* boot self test (background, results in log) */
    event_log("SYSTEM", "Logging started; interval %d s",
              g_cfg.store_interval);

    while (1) {
        uint32_t wait = lv_timer_handler();
        if (wait > 30) wait = 30;
        if (wait < 1)  wait = 1;
        sleep_ms(wait);
    }
    return 0;
}
