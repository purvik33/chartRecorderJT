/* comm_modbus.c - Modbus RTU master polling iAI_U8 cards over RS-485.
 * Portable serial: Win32 COM port or POSIX termios (USB-RS485 dongle
 * on Windows, /dev/ttyUSB0 on the Pi).
 *
 * Assumed card map (adjust reg_base/word_order in recorder.ini):
 *   FC03, reg_base .. reg_base+15  =  8 x float32 channel values
 * Card N is Modbus slave (slave_base + N - 1). */
#include "comm.h"
#include "config.h"
#include "data_model.h"
#include "alarm.h"
#include "events.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE serial_t;
#define SERIAL_BAD INVALID_HANDLE_VALUE
static void msleep(int ms) { Sleep(ms); }

static serial_t serial_open(const char *port, int baud)
{
    char path[48];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return SERIAL_BAD;

    DCB dcb = { .DCBlength = sizeof(DCB) };
    GetCommState(h, &dcb);
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);

    COMMTIMEOUTS to = { .ReadIntervalTimeout = 20,
                        .ReadTotalTimeoutConstant = 200,
                        .ReadTotalTimeoutMultiplier = 2,
                        .WriteTotalTimeoutConstant = 200 };
    SetCommTimeouts(h, &to);
    return h;
}
static int serial_write(serial_t h, const uint8_t *buf, int n)
{
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    return (int)w;
}
static int serial_read(serial_t h, uint8_t *buf, int n)
{
    DWORD r = 0;
    ReadFile(h, buf, (DWORD)n, &r, NULL);
    return (int)r;
}
static void serial_flush(serial_t h) { PurgeComm(h, PURGE_RXCLEAR); }
static void serial_close(serial_t h) { CloseHandle(h); }

#else /* POSIX */
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
typedef int serial_t;
#define SERIAL_BAD (-1)
static void msleep(int ms) { usleep(ms * 1000); }

static serial_t serial_open(const char *port, int baud)
{
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) return SERIAL_BAD;

    speed_t sp = B9600;
    switch (baud) {
    case 19200:  sp = B19200;  break;
    case 38400:  sp = B38400;  break;
    case 57600:  sp = B57600;  break;
    case 115200: sp = B115200; break;
    }
    struct termios tio;
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 2;   /* 200 ms read timeout */
    tcsetattr(fd, TCSANOW, &tio);
    return fd;
}
static int serial_write(serial_t fd, const uint8_t *buf, int n)
{
    return (int)write(fd, buf, (size_t)n);
}
static int serial_read(serial_t fd, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = (int)read(fd, buf + got, (size_t)(n - got));
        if (r <= 0) break;
        got += r;
    }
    return got;
}
static void serial_flush(serial_t fd) { tcflush(fd, TCIFLUSH); }
static void serial_close(serial_t fd) { close(fd); }
#endif

/* ---- Modbus RTU ------------------------------------------------------ */

static volatile int link_ok;
int comm_modbus_link_ok(void) { return link_ok; }

/* serial handle shared by the poll thread and the service API;
 * every bus transaction holds bus_mtx so they never interleave */
static serial_t bus = SERIAL_BAD;
static pthread_mutex_t bus_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint16_t crc16(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/* FC03/FC04 register read; returns 0 on success */
static int mb_read_regs(serial_t h, int slave, int fc, int addr, int nreg,
                        uint16_t *out)
{
    uint8_t req[8];
    req[0] = (uint8_t)slave;
    req[1] = (uint8_t)fc;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(nreg >> 8);
    req[5] = (uint8_t)(nreg & 0xFF);
    uint16_t crc = crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    serial_flush(h);
    if (serial_write(h, req, 8) != 8) return -1;

    int explen = 5 + 2 * nreg;   /* id fc bc data.. crc2 */
    uint8_t rsp[256];
    if (explen > (int)sizeof(rsp)) return -1;
    if (serial_read(h, rsp, explen) != explen) return -1;

    if (rsp[0] != slave || rsp[1] != fc || rsp[2] != 2 * nreg) return -1;
    uint16_t rcrc = (uint16_t)(rsp[explen - 1] << 8 | rsp[explen - 2]);
    if (crc16(rsp, explen - 2) != rcrc) return -1;

    for (int i = 0; i < nreg; i++)
        out[i] = (uint16_t)(rsp[3 + 2 * i] << 8 | rsp[4 + 2 * i]);
    return 0;
}

/* FC06 write single register; returns 0 on success (echo verified) */
static int mb_write_reg(serial_t h, int slave, int addr, uint16_t val)
{
    uint8_t req[8];
    req[0] = (uint8_t)slave;
    req[1] = 0x06;
    req[2] = (uint8_t)(addr >> 8);
    req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(val >> 8);
    req[5] = (uint8_t)(val & 0xFF);
    uint16_t crc = crc16(req, 6);
    req[6] = (uint8_t)(crc & 0xFF);
    req[7] = (uint8_t)(crc >> 8);

    serial_flush(h);
    if (serial_write(h, req, 8) != 8) return -1;

    uint8_t rsp[8];
    if (serial_read(h, rsp, 8) != 8) return -1;
    if (memcmp(req, rsp, 8) != 0) return -1;   /* echo must match */
    return 0;
}

int mb_service_read(int slave, int fc, int addr, int n, uint16_t *out)
{
    if (g_cfg.source != SRC_MODBUS || n < 1 || n > 32) return -1;
    pthread_mutex_lock(&bus_mtx);
    int rc = (bus == SERIAL_BAD) ? -1
             : mb_read_regs(bus, slave, fc, addr, n, out);
    pthread_mutex_unlock(&bus_mtx);
    return rc;
}

int mb_service_write(int slave, int addr, uint16_t val)
{
    if (g_cfg.source != SRC_MODBUS) return -1;
    pthread_mutex_lock(&bus_mtx);
    int rc = (bus == SERIAL_BAD) ? -1
             : mb_write_reg(bus, slave, addr, val);
    pthread_mutex_unlock(&bus_mtx);
    return rc;
}

/* Bus-wide baud change. Sequence per iAI_U8 firmware: the card replies
 * to the save-key write at the OLD baud, waits 50 ms for the response
 * to drain, then re-inits its UART at the new rate — so all cards are
 * programmed first, then our own port follows. The whole sequence runs
 * under bus_mtx so the poll thread can't interleave. */
int comm_bus_baud_change(int new_baud, char *report, int rep_n)
{
    static const struct { int baud; uint16_t code; } tab[] = {
        { 9600, 1 }, { 19200, 2 }, { 38400, 3 }, { 57600, 4 }, { 115200, 5 }
    };
    uint16_t code = 0;
    for (int i = 0; i < 5; i++)
        if (tab[i].baud == new_baud) code = tab[i].code;

    if (report && rep_n > 0) report[0] = '\0';
    if (!code || g_cfg.source != SRC_MODBUS) {
        if (report) snprintf(report, rep_n, "Invalid rate / demo mode");
        return -1;
    }

    pthread_mutex_lock(&bus_mtx);

    if (bus == SERIAL_BAD) {
        pthread_mutex_unlock(&bus_mtx);
        if (report)
            snprintf(report, rep_n, "No card link - recorder port"
                                    " rate changed only");
        return -1;
    }

    /* phase 1: program every card at the current baud */
    int programmed = 0;
    for (int card = 0; card < g_cfg.cards; card++) {
        int slave = g_cfg.slave_base + card;
        if (mb_write_reg(bus, slave, REG_COMM_BAUD, code) == 0 &&
            mb_write_reg(bus, slave, REG_COMM_SAVE, 1221) == 0)
            programmed |= 1 << card;
    }

    /* phase 2: let the cards re-init, then reopen our port to match */
    msleep(200);
    serial_close(bus);
    bus = serial_open(g_cfg.port, new_baud);
    if (bus == SERIAL_BAD) {
        pthread_mutex_unlock(&bus_mtx);
        if (report) snprintf(report, rep_n, "Port re-open failed");
        return -1;
    }

    /* phase 3: verify each card answers at the new rate */
    int pos = 0, ok_cnt = 0;
    for (int card = 0; card < g_cfg.cards; card++) {
        int slave = g_cfg.slave_base + card;
        uint16_t v = 0;
        int ok = 0;
        for (int try = 0; try < 2 && !ok; try++)
            ok = (mb_read_regs(bus, slave, 3, REG_COMM_BAUD, 1, &v) == 0 &&
                  v == code);
        if (ok) ok_cnt++;
        if (report && pos < rep_n)
            pos += snprintf(report + pos, (size_t)(rep_n - pos),
                            "%sCard %d: %s", card ? "   " : "", card + 1,
                            ok ? "OK" : (programmed & (1 << card))
                                        ? "no reply" : "not programmed");
    }

    pthread_mutex_unlock(&bus_mtx);
    return ok_cnt;
}

/* mINT-08AI input type table, document m25A/om/101 issue 11 */
const char *mb_type_name(unsigned code)
{
    static const char *names[] = {
        "Skip",      /* 0  channel off            */
        "TC E",      /* 1  -200..1000 C           */
        "TC J",      /* 2  -200..1200 C           */
        "TC K",      /* 3  -200..1350 C           */
        "TC T",      /* 4  -200..400 C            */
        "TC B",      /* 5   450..1800 C           */
        "TC R",      /* 6   0..1750 C             */
        "TC S",      /* 7   0..1750 C             */
        "TC N",      /* 8  -200..1300 C           */
        "Pt-100",    /* 9  -200..850 C, 3-wire    */
        "Cu-53",     /* 10 -210..210 C            */
        "Ni-120",    /* 11 -80..210 C             */
        "0-20 mA",   /* 12 */
        "4-20 mA",   /* 13 */
        "Ohms",      /* 14 0..2000 ohm            */
        "50 mV",     /* 15 -10..+50 mV            */
        "100 mV",    /* 16 0..100 mV              */
        "250 mV",    /* 17 0..250 mV              */
        "0-5 V",     /* 18 */
        "0-10 V",    /* 19 */
    };
    return code < 20 ? names[code] : "Unknown";
}

static void type_to_name(uint16_t code, char *out, int n)
{
    snprintf(out, n, "%s", mb_type_name(code));
}

void comm_refresh_types(int card)
{
    uint16_t t[CH_PER_GROUP], dec[CH_PER_GROUP];
    if (mb_service_read(g_cfg.slave_base + card, 3, REG_TYPE,
                        CH_PER_GROUP, t) != 0) return;
    int dec_ok = (mb_service_read(g_cfg.slave_base + card, 3, REG_DECIMALS,
                                  CH_PER_GROUP, dec) == 0);
    data_lock();
    for (int c = 0; c < CH_PER_GROUP; c++) {
        channel_t *ch = &g_ch[card * CH_PER_GROUP + c];
        type_to_name(t[c], ch->sensor, (int)sizeof(ch->sensor));
        /* scaling: temperature types are x10; V/I use the card's
         * decimals register (value = counts / 10^dec) */
        if (t[c] >= 12 && t[c] <= 19) {
            int d = (dec_ok && dec[c] <= 5) ? (int)dec[c] : 3;
            float dv = 1.0f;
            while (d-- > 0) dv *= 10.0f;
            ch->div = dv;
        } else {
            ch->div = 10.0f;
            /* RTD/TC always measure in degrees Celsius */
            if (t[c] >= 1 && t[c] <= 11)
                snprintf(ch->unit, sizeof(ch->unit), "\xC2\xB0""C");
        }
    }
    data_unlock();
}

static float regs_to_float(const uint16_t *r)
{
    union { uint32_t u; float f; } v;
    if (g_cfg.word_order == 1)          /* ABCD: high word first */
        v.u = ((uint32_t)r[0] << 16) | r[1];
    else                                /* CDAB: low word first */
        v.u = ((uint32_t)r[1] << 16) | r[0];
    return v.f;
}

void *comm_modbus_thread(void *arg)
{
    (void)arg;

    while (1) {
        if (bus == SERIAL_BAD) {
            serial_t h = serial_open(g_cfg.port, g_cfg.baud);
            if (h == SERIAL_BAD) {
                link_ok = 0;
                data_lock();
                for (int i = 0; i < g_cfg.cards * CH_PER_GROUP; i++)
                    g_ch[i].status = CH_COMM;
                data_unlock();
                alarm_eval();
                msleep(2000);
                continue;
            }
            bus = h;
            /* new connection: fetch each card's input types */
            for (int card = 0; card < g_cfg.cards; card++)
                comm_refresh_types(card);
        }

        int any_ok = 0;
        int nreg = (g_cfg.fmt == FMT_FLOAT) ? 16 : 8;
        float scale = (g_cfg.fmt == FMT_I16_10)  ? 10.0f :
                      (g_cfg.fmt == FMT_I16_100) ? 100.0f : 1.0f;

        for (int card = 0; card < g_cfg.cards; card++) {
            uint16_t regs[16];
            int slave = g_cfg.slave_base + card;
            pthread_mutex_lock(&bus_mtx);
            int ok = (bus != SERIAL_BAD &&
                      mb_read_regs(bus, slave, g_cfg.func,
                                   g_cfg.reg_base, nreg, regs) == 0);
            pthread_mutex_unlock(&bus_mtx);

            data_lock();
            for (int c = 0; c < CH_PER_GROUP; c++) {
                channel_t *ch = &g_ch[card * CH_PER_GROUP + c];
                if (!ok) {
                    ch->status = CH_COMM;
                    continue;
                }
                if (g_cfg.fmt == FMT_FLOAT) {
                    ch->value = regs_to_float(&regs[c * 2]);
                } else {
                    int16_t raw = (int16_t)regs[c];
                    if (raw == 32764) { ch->status = CH_SKIP;  continue; }
                    if (raw == 32765) { ch->status = CH_UNDER; continue; }
                    if (raw == 32766) { ch->status = CH_OVER;  continue; }
                    if (raw == 32767) { ch->status = CH_OPEN;  continue; }
                    ch->value = (float)raw /
                                (ch->div > 0 ? ch->div : scale);
                }
                if (ch->status == CH_COMM || ch->status == CH_OPEN ||
                    ch->status == CH_SKIP || ch->status == CH_UNDER ||
                    ch->status == CH_OVER)
                    ch->status = CH_OK;
            }
            data_unlock();
            if (ok) any_ok = 1;
        }

        /* log link transitions to the event trail */
        {
            static int prev_link = -1;
            if (any_ok != prev_link) {
                if (prev_link != -1 || any_ok)
                    event_log("COMM", any_ok ? "RS-485 link up"
                                             : "RS-485 link down");
                prev_link = any_ok;
            }
        }

        link_ok = any_ok;
        if (!any_ok) {
            pthread_mutex_lock(&bus_mtx);
            if (bus != SERIAL_BAD) { serial_close(bus); bus = SERIAL_BAD; }
            pthread_mutex_unlock(&bus_mtx);
        }

        alarm_eval();
        data_live_push();
        msleep(500);
    }
    return NULL;
}
