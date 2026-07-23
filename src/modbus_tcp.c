/* modbus_tcp.c - Modbus TCP server + transparent RS-485 gateway.
 * Single thread, select() over the listener and up to 4 clients, so a
 * stalled client can never block the recorder. Register map and unit
 * id routing are described in modbus_tcp.h. */
#include "modbus_tcp.h"
#include "comm.h"
#include "config.h"
#include "data_model.h"
#include "alarm.h"
#include "events.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
static void tcp_msleep(int ms) { Sleep(ms); }
static int  sock_init(void)
{
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
}
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
static void tcp_msleep(int ms) { usleep(ms * 1000); }
static int  sock_init(void) { return 0; }
#endif

#define MAX_CLI      4
#define GW_UNIT_BASE 11        /* unit 11..15 -> card 1..5 */

static volatile int cli_count;
int modbus_tcp_clients(void) { return cli_count; }

/* ---- recorder's own register map ------------------------------------- */

static int st_code(ch_status_t s)
{
    switch (s) {
    case CH_ALM_HI: return 1;
    case CH_ALM_LO: return 2;
    case CH_SKIP:   return 3;
    case CH_UNDER:  return 4;
    case CH_OVER:   return 5;
    case CH_OPEN:   return 6;
    case CH_COMM:   return 7;
    default:        return 0;
    }
}

static uint16_t x10(float v)
{
    float s = v * 10.0f;
    if (s >  32763.0f) s =  32763.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (uint16_t)(int16_t)s;
}

/* one input register; returns 0 ok, -1 illegal address */
static int local_input_reg(int addr, uint16_t *out)
{
    *out = 0;
    if (addr >= 0 && addr < CH_TOTAL) {              /* PV x10 + codes */
        data_lock();
        channel_t *c = &g_ch[addr];
        switch (c->status) {
        case CH_SKIP:  *out = 32764; break;
        case CH_UNDER: *out = 32765; break;
        case CH_OVER:  *out = 32766; break;
        case CH_OPEN:
        case CH_COMM:  *out = 32767; break;
        default:       *out = x10(c->value); break;
        }
        data_unlock();
        return 0;
    }
    if (addr >= 100 && addr < 100 + 2 * CH_TOTAL) {  /* PV float32 */
        int ch = (addr - 100) / 2, hi_word = ((addr - 100) & 1) == 0;
        union { float f; uint32_t u; } v;
        data_lock();
        v.f = g_ch[ch].value;
        data_unlock();
        if (g_cfg.word_order == 1)   /* ABCD: first reg = high word */
            *out = hi_word ? (uint16_t)(v.u >> 16) : (uint16_t)v.u;
        else                         /* CDAB: first reg = low word */
            *out = hi_word ? (uint16_t)v.u : (uint16_t)(v.u >> 16);
        return 0;
    }
    if (addr >= 200 && addr < 200 + CH_TOTAL) {      /* status codes */
        data_lock();
        *out = (uint16_t)st_code(g_ch[addr - 200].status);
        data_unlock();
        return 0;
    }
    switch (addr) {
    case 240: *out = (uint16_t)comm_link_ok();            return 0;
    case 241: *out = (uint16_t)alarm_active_count();      return 0;
    case 242: *out = (uint16_t)alarm_unacked_count();     return 0;
    case 243: *out = (uint16_t)g_cfg.store_interval;      return 0;
    case 244: *out = (uint16_t)g_cfg.cards;               return 0;
    }
    return -1;
}

static int local_holding_reg(int addr, uint16_t *out)
{
    *out = 0;
    if (addr >= 0 && addr < CH_TOTAL) {
        data_lock();
        *out = x10(g_ch[addr].alm_hi);
        data_unlock();
        return 0;
    }
    if (addr >= 40 && addr < 40 + CH_TOTAL) {
        data_lock();
        *out = x10(g_ch[addr - 40].alm_lo);
        data_unlock();
        return 0;
    }
    if (addr == 100) { *out = (uint16_t)g_cfg.store_interval; return 0; }
    if (addr == 101) { *out = 0; return 0; }
    return -1;
}

/* returns 0 ok, -1 illegal address/value; `dirty` set when the config
 * must be persisted after the request completes */
static int local_write_reg(int addr, uint16_t val, int *dirty)
{
    if (addr >= 0 && addr < CH_TOTAL) {
        float v = (float)(int16_t)val / 10.0f;
        data_lock();
        g_ch[addr].alm_hi = v;
        data_unlock();
        event_log("COMM", "TCP write: CH%d alarm HIGH = %.1f", addr + 1,
                  (double)v);
        *dirty = 1;
        return 0;
    }
    if (addr >= 40 && addr < 40 + CH_TOTAL) {
        float v = (float)(int16_t)val / 10.0f;
        data_lock();
        g_ch[addr - 40].alm_lo = v;
        data_unlock();
        event_log("COMM", "TCP write: CH%d alarm LOW = %.1f", addr - 39,
                  (double)v);
        *dirty = 1;
        return 0;
    }
    if (addr == 100) {
        int iv = (int)val;
        if (iv < 60 || iv > 3600) return -1;   /* 1 min .. 1 h */
        g_cfg.store_interval = iv;
        event_log("COMM", "TCP write: store interval = %d s", iv);
        *dirty = 1;
        return 0;
    }
    if (addr == 101) {
        if (val == 1221) {
            alarm_ack_all();
            event_log("ALARM", "All alarms acknowledged (TCP)");
        }
        return 0;
    }
    return -1;
}

/* ---- PDU handling ----------------------------------------------------- */

static int exception(uint8_t fc, uint8_t code, uint8_t *rsp)
{
    rsp[0] = (uint8_t)(fc | 0x80);
    rsp[1] = code;
    return 2;
}

/* handle one request PDU; returns response PDU length */
static int process_pdu(int unit, const uint8_t *pdu, int plen, uint8_t *rsp)
{
    if (plen < 1) return 0;
    uint8_t fc = pdu[0];

    /* ---- gateway: forward to an AI card over RS-485 ---- */
    /* the configured local unit always serves the recorder's own map,
     * even when tcp_unit falls inside the gateway range 11..15 */
    if (unit != g_cfg.tcp_unit &&
        unit >= GW_UNIT_BASE && unit < GW_UNIT_BASE + g_cfg.cards) {
        int slave = g_cfg.slave_base + (unit - GW_UNIT_BASE);
        if ((fc == 3 || fc == 4) && plen >= 5) {
            int addr = pdu[1] << 8 | pdu[2];
            int n    = pdu[3] << 8 | pdu[4];
            uint16_t buf[32];
            if (n < 1 || n > 32) return exception(fc, 0x03, rsp);
            if (mb_service_read(slave, fc, addr, n, buf) != 0)
                return exception(fc, 0x0B, rsp);   /* gateway target fail */
            rsp[0] = fc;
            rsp[1] = (uint8_t)(2 * n);
            for (int i = 0; i < n; i++) {
                rsp[2 + 2 * i] = (uint8_t)(buf[i] >> 8);
                rsp[3 + 2 * i] = (uint8_t)buf[i];
            }
            return 2 + 2 * n;
        }
        if (fc == 6 && plen >= 5) {
            int addr     = pdu[1] << 8 | pdu[2];
            uint16_t val = (uint16_t)(pdu[3] << 8 | pdu[4]);
            if (mb_service_write(slave, addr, val) != 0)
                return exception(fc, 0x0B, rsp);
            event_log("COMM", "TCP gateway write: card %d reg %d = %u",
                      unit - GW_UNIT_BASE + 1, addr, val);
            memcpy(rsp, pdu, 5);
            return 5;
        }
        return exception(fc, 0x01, rsp);           /* illegal function */
    }

    if (unit != g_cfg.tcp_unit)
        return exception(fc, 0x0B, rsp);           /* no such unit */

    /* ---- the recorder's own map ---- */
    if ((fc == 3 || fc == 4) && plen >= 5) {
        int addr = pdu[1] << 8 | pdu[2];
        int n    = pdu[3] << 8 | pdu[4];
        if (n < 1 || n > 100) return exception(fc, 0x03, rsp);
        rsp[0] = fc;
        rsp[1] = (uint8_t)(2 * n);
        for (int i = 0; i < n; i++) {
            uint16_t v;
            int ok = (fc == 4) ? local_input_reg(addr + i, &v)
                               : local_holding_reg(addr + i, &v);
            if (ok != 0) return exception(fc, 0x02, rsp);
            rsp[2 + 2 * i] = (uint8_t)(v >> 8);
            rsp[3 + 2 * i] = (uint8_t)v;
        }
        return 2 + 2 * n;
    }
    if (fc == 6 && plen >= 5) {
        int addr     = pdu[1] << 8 | pdu[2];
        uint16_t val = (uint16_t)(pdu[3] << 8 | pdu[4]);
        int dirty = 0;
        if (local_write_reg(addr, val, &dirty) != 0)
            return exception(fc, 0x02, rsp);
        if (dirty) config_save();
        memcpy(rsp, pdu, 5);
        return 5;
    }
    if (fc == 16 && plen >= 6) {
        int addr = pdu[1] << 8 | pdu[2];
        int n    = pdu[3] << 8 | pdu[4];
        int bc   = pdu[5];
        if (n < 1 || n > 100 || bc != 2 * n || plen < 6 + bc)
            return exception(fc, 0x03, rsp);
        int dirty = 0;
        for (int i = 0; i < n; i++) {
            uint16_t val = (uint16_t)(pdu[6 + 2 * i] << 8 | pdu[7 + 2 * i]);
            if (local_write_reg(addr + i, val, &dirty) != 0)
                return exception(fc, 0x02, rsp);
        }
        if (dirty) config_save();
        rsp[0] = fc;
        memcpy(rsp + 1, pdu + 1, 4);
        return 5;
    }
    return exception(fc, 0x01, rsp);
}

/* ---- TCP server ------------------------------------------------------- */

typedef struct {
    sock_t  s;
    uint8_t buf[300];
    int     len;
} client_t;

static void client_data(client_t *c)
{
    /* MBAP: tid(2) pid(2) len(2) uid(1) pdu... */
    while (c->len >= 7) {
        int flen = (c->buf[4] << 8 | c->buf[5]);   /* uid + pdu */
        if (flen < 2 || flen > 260) { c->len = 0; return; }
        if (c->len < 6 + flen) return;             /* incomplete */

        uint8_t rsp[280];
        memcpy(rsp, c->buf, 7);                    /* tid/pid/uid kept */
        int rlen = process_pdu(c->buf[6], c->buf + 7, flen - 1, rsp + 7);
        if (rlen > 0) {
            rsp[4] = (uint8_t)((rlen + 1) >> 8);
            rsp[5] = (uint8_t)((rlen + 1) & 0xFF);
            send(c->s, (const char *)rsp, 7 + rlen, 0);
        }
        memmove(c->buf, c->buf + 6 + flen, (size_t)(c->len - 6 - flen));
        c->len -= 6 + flen;
    }
}

static void *tcp_thread(void *arg)
{
    (void)arg;
    if (sock_init() != 0) return NULL;

    sock_t   lst = SOCK_BAD;
    int      lst_port = -1;
    client_t cli[MAX_CLI];
    for (int i = 0; i < MAX_CLI; i++) cli[i].s = SOCK_BAD;

    while (1) {
        /* follow the enable switch and port live */
        if (!g_cfg.tcp_enable || lst_port != g_cfg.tcp_port) {
            if (lst != SOCK_BAD) { sock_close(lst); lst = SOCK_BAD; }
            lst_port = -1;
        }
        if (!g_cfg.tcp_enable) {
            for (int i = 0; i < MAX_CLI; i++)
                if (cli[i].s != SOCK_BAD) {
                    sock_close(cli[i].s);
                    cli[i].s = SOCK_BAD;
                }
            cli_count = 0;
            tcp_msleep(1000);
            continue;
        }
        if (lst == SOCK_BAD) {
            lst = socket(AF_INET, SOCK_STREAM, 0);
            if (lst != SOCK_BAD) {
                int yes = 1;
                setsockopt(lst, SOL_SOCKET, SO_REUSEADDR,
                           (const char *)&yes, sizeof(yes));
                struct sockaddr_in a;
                memset(&a, 0, sizeof(a));
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = INADDR_ANY;
                a.sin_port = htons((uint16_t)g_cfg.tcp_port);
                if (bind(lst, (struct sockaddr *)&a, sizeof(a)) != 0 ||
                    listen(lst, 2) != 0) {
                    sock_close(lst);
                    lst = SOCK_BAD;
                }
            }
            if (lst == SOCK_BAD) { tcp_msleep(2000); continue; }
            lst_port = g_cfg.tcp_port;
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lst, &rd);
        sock_t maxfd = lst;
        for (int i = 0; i < MAX_CLI; i++)
            if (cli[i].s != SOCK_BAD) {
                FD_SET(cli[i].s, &rd);
                if (cli[i].s > maxfd) maxfd = cli[i].s;
            }

        struct timeval tv = { 0, 500 * 1000 };
        int r = select((int)maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0) { tcp_msleep(200); continue; }
        if (r == 0) continue;

        if (FD_ISSET(lst, &rd)) {
            sock_t ns = accept(lst, NULL, NULL);
            if (ns != SOCK_BAD) {
                int slot = -1;
                for (int i = 0; i < MAX_CLI && slot < 0; i++)
                    if (cli[i].s == SOCK_BAD) slot = i;
                if (slot < 0) sock_close(ns);   /* table full */
                else {
                    int yes = 1;
                    setsockopt(ns, IPPROTO_TCP, TCP_NODELAY,
                               (const char *)&yes, sizeof(yes));
                    cli[slot].s = ns;
                    cli[slot].len = 0;
                }
            }
        }
        for (int i = 0; i < MAX_CLI; i++) {
            if (cli[i].s == SOCK_BAD || !FD_ISSET(cli[i].s, &rd)) continue;
            int n = recv(cli[i].s, (char *)cli[i].buf + cli[i].len,
                         (int)sizeof(cli[i].buf) - cli[i].len, 0);
            if (n <= 0) {
                sock_close(cli[i].s);
                cli[i].s = SOCK_BAD;
                continue;
            }
            cli[i].len += n;
            client_data(&cli[i]);
        }

        int cc = 0;
        for (int i = 0; i < MAX_CLI; i++)
            if (cli[i].s != SOCK_BAD) cc++;
        cli_count = cc;
    }
    return NULL;
}

void modbus_tcp_init(void)
{
    pthread_t t;
    pthread_create(&t, NULL, tcp_thread, NULL);
}

/* ---- network helpers -------------------------------------------------- */

void net_current_ip(char *buf, int n)
{
    snprintf(buf, (size_t)n, "-");
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_BAD) return;
    /* connect() on UDP sends nothing; it just selects the local route */
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x08080808);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        struct sockaddr_in loc;
        socklen_t ll = sizeof(loc);
        if (getsockname(s, (struct sockaddr *)&loc, &ll) == 0) {
            const uint8_t *p = (const uint8_t *)&loc.sin_addr.s_addr;
            snprintf(buf, (size_t)n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
        }
    }
    sock_close(s);
}

#if !defined(_WIN32) && defined(RECORDER_PI)
/* Run argv (NUL-terminated) directly via fork()+execvp()+waitpid() with
 * NO shell in between, so config values passed as separate argv elements
 * can never be interpreted as shell syntax. Runs synchronously and
 * returns the child's exit status (0 = success), or -1 on spawn failure. */
static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);                    /* execvp failed */
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Resolve the NetworkManager connection currently bound to `dev`
 * (e.g. "eth0") into `out`. Returns 0 on success, -1 if none is found.
 * The command is fixed with no user input, so popen() is safe here. */
static int nm_conn_for_dev(const char *dev, char *out, size_t outsz)
{
    FILE *p = popen("nmcli -t -f DEVICE,CONNECTION dev status 2>/dev/null",
                    "r");
    if (!p) return -1;
    char line[256];
    int found = -1;
    size_t dl = strlen(dev);
    while (fgets(line, sizeof(line), p)) {
        /* format: DEVICE:CONNECTION */
        if (strncmp(line, dev, dl) == 0 && line[dl] == ':') {
            char *conn = line + dl + 1;
            conn[strcspn(conn, "\r\n")] = 0;
            if (conn[0] && strcmp(conn, "--") != 0) {
                snprintf(out, outsz, "%s", conn);
                found = 0;
            }
            break;
        }
    }
    pclose(p);
    return found;
}
#endif

void net_apply(void)
{
#if !defined(_WIN32) && defined(RECORDER_PI)
    /* Raspberry Pi OS (bookworm) uses NetworkManager. Resolve the eth0
     * connection at runtime instead of assuming "Wired connection 1";
     * on netplan-managed Pis it is named e.g. "netplan-eth0". */
    char conn[128];
    if (nm_conn_for_dev("eth0", conn, sizeof(conn)) != 0) {
        event_log("CONFIG", "Network apply FAILED: no connection for eth0");
        return;
    }
    int rc;
    if (g_cfg.net_dhcp) {
        char *mod[] = { "nmcli", "con", "mod", conn,
                        "ipv4.method", "auto", NULL };
        rc = run_argv(mod);
    } else {
        /* mask string -> prefix length */
        unsigned m[4] = { 0, 0, 0, 0 };
        sscanf(g_cfg.net_mask, "%u.%u.%u.%u", &m[0], &m[1], &m[2], &m[3]);
        uint32_t mm = m[0] << 24 | m[1] << 16 | m[2] << 8 | m[3];
        int prefix = 0;
        while (mm & 0x80000000u) { prefix++; mm <<= 1; }
        char addr[32];
        snprintf(addr, sizeof(addr), "%s/%d", g_cfg.net_ip, prefix);
        char *mod[] = { "nmcli", "con", "mod", conn,
                        "ipv4.method", "manual",
                        "ipv4.addresses", addr,
                        "ipv4.gateway", g_cfg.net_gw,
                        "ipv4.dns", g_cfg.net_dns, NULL };
        rc = run_argv(mod);
    }
    if (rc == 0) {
        char *up[] = { "nmcli", "con", "up", conn, NULL };
        rc = run_argv(up);
    }
    event_log("CONFIG", "Network settings %s (%s)",
              rc == 0 ? "applied" : "apply FAILED",
              g_cfg.net_dhcp ? "DHCP" : g_cfg.net_ip);
#else
    /* simulator: settings are stored and take effect on the recorder */
    event_log("CONFIG", "Network settings saved (%s)",
              g_cfg.net_dhcp ? "DHCP" : g_cfg.net_ip);
#endif
}

int wifi_scan(char (*out)[33], int max)
{
    int n = 0;
#ifdef _WIN32
    FILE *p = _popen("netsh wlan show networks", "r");
#else
    FILE *p = popen("nmcli -t -f SSID dev wifi list --rescan yes"
                    " 2>/dev/null", "r");
#endif
    if (!p) return 0;

    char line[256];
    while (fgets(line, sizeof(line), p) && n < max) {
        char ssid[64] = "";
#ifdef _WIN32
        /* lines look like "SSID 1 : MyNetwork" (skip BSSID rows) */
        if (strncmp(line, "SSID", 4) != 0) continue;
        char *c = strchr(line, ':');
        if (!c) continue;
        c++;
        while (*c == ' ') c++;
        snprintf(ssid, sizeof(ssid), "%s", c);
#else
        snprintf(ssid, sizeof(ssid), "%s", line);
#endif
        ssid[strcspn(ssid, "\r\n")] = 0;
        if (!ssid[0]) continue;

        int dup = 0;
        for (int i = 0; i < n && !dup; i++)
            if (!strcmp(out[i], ssid)) dup = 1;
        if (dup) continue;

        snprintf(out[n], 33, "%s", ssid);
        n++;
    }
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return n;
}

void wifi_apply(void)
{
#if !defined(_WIN32) && defined(RECORDER_PI)
    int rc = 0;
    if (!g_cfg.wifi_enable) {
        char *off[] = { "nmcli", "radio", "wifi", "off", NULL };
        rc = run_argv(off);
    } else {
        char *on[] = { "nmcli", "radio", "wifi", "on", NULL };
        rc = run_argv(on);
        if (rc == 0) {
            /* `nmcli dev wifi connect` names the resulting profile after
             * the SSID; SSID/password are passed as separate argv elements
             * so they are never parsed as shell syntax */
            char *conn[] = { "nmcli", "dev", "wifi", "connect",
                             g_cfg.wifi_ssid, "password", g_cfg.wifi_pass,
                             NULL };
            rc = run_argv(conn);
        }
        if (rc == 0 && !g_cfg.wifi_dhcp) {
            unsigned m[4] = { 0, 0, 0, 0 };
            sscanf(g_cfg.wifi_mask, "%u.%u.%u.%u",
                   &m[0], &m[1], &m[2], &m[3]);
            uint32_t mm = m[0] << 24 | m[1] << 16 | m[2] << 8 | m[3];
            int prefix = 0;
            while (mm & 0x80000000u) { prefix++; mm <<= 1; }
            char addr[32];
            snprintf(addr, sizeof(addr), "%s/%d", g_cfg.wifi_ip, prefix);
            char *mod[] = { "nmcli", "con", "mod", g_cfg.wifi_ssid,
                            "ipv4.method", "manual",
                            "ipv4.addresses", addr,
                            "ipv4.gateway", g_cfg.wifi_gw, NULL };
            rc = run_argv(mod);
            if (rc == 0) {
                char *up[] = { "nmcli", "con", "up", g_cfg.wifi_ssid, NULL };
                rc = run_argv(up);
            }
        }
    }
    event_log("CONFIG", "Wi-Fi %s (%s)%s",
              g_cfg.wifi_enable ? "connecting" : "disabled",
              g_cfg.wifi_enable ? g_cfg.wifi_ssid : "-",
              rc == 0 ? "" : " - FAILED");
#else
    event_log("CONFIG", "Wi-Fi settings saved (%s, %s)",
              g_cfg.wifi_enable ? "enabled" : "disabled",
              g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "-");
#endif
}
