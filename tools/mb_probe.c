/* mb_probe.c - standalone Modbus RTU probe for finding a card's
 * slave id, baud rate, function code and register layout.
 * Build:  gcc mb_probe.c -o mb_probe
 * Usage:  mb_probe COM13            (scan)
 *         mb_probe COM13 9600 1 3 0 16   (single read: baud slave fc addr n) */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static HANDLE ser_open(const char *port, int baud)
{
    char path[48];
    snprintf(path, sizeof(path), "\\\\.\\%s", port);
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    DCB dcb = { .DCBlength = sizeof(DCB) };
    GetCommState(h, &dcb);
    dcb.BaudRate = baud; dcb.ByteSize = 8;
    dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    SetCommState(h, &dcb);
    COMMTIMEOUTS to = { .ReadIntervalTimeout = 20,
                        .ReadTotalTimeoutConstant = 150,
                        .ReadTotalTimeoutMultiplier = 1,
                        .WriteTotalTimeoutConstant = 150 };
    SetCommTimeouts(h, &to);
    return h;
}

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

static int mb_read(HANDLE h, int slave, int fc, int addr, int nreg,
                   uint16_t *out)
{
    uint8_t req[8];
    req[0] = (uint8_t)slave; req[1] = (uint8_t)fc;
    req[2] = (uint8_t)(addr >> 8); req[3] = (uint8_t)(addr & 0xFF);
    req[4] = (uint8_t)(nreg >> 8); req[5] = (uint8_t)(nreg & 0xFF);
    uint16_t c = crc16(req, 6);
    req[6] = (uint8_t)(c & 0xFF); req[7] = (uint8_t)(c >> 8);

    PurgeComm(h, PURGE_RXCLEAR);
    DWORD w;
    WriteFile(h, req, 8, &w, NULL);

    int explen = 5 + 2 * nreg;
    uint8_t rsp[300];
    DWORD r = 0;
    ReadFile(h, rsp, (DWORD)explen, &r, NULL);
    if ((int)r < 5) return -1;
    if (rsp[0] != slave) return -2;
    if (rsp[1] == (fc | 0x80)) return rsp[2];   /* modbus exception code */
    if ((int)r != explen || rsp[2] != 2 * nreg) return -3;
    uint16_t rc = (uint16_t)(rsp[explen-1] << 8 | rsp[explen-2]);
    if (crc16(rsp, explen - 2) != rc) return -4;
    for (int i = 0; i < nreg; i++)
        out[i] = (uint16_t)(rsp[3 + 2*i] << 8 | rsp[4 + 2*i]);
    return 0;
}

static void dump(int slave, int fc, int addr, uint16_t *r, int n)
{
    printf("  regs[%d..%d] fc%d slave %d:\n  hex:", addr, addr+n-1, fc, slave);
    for (int i = 0; i < n; i++) printf(" %04X", r[i]);
    printf("\n  u16:");
    for (int i = 0; i < n; i++) printf(" %u", r[i]);
    printf("\n  float CDAB:");
    for (int i = 0; i + 1 < n; i += 2) {
        union { uint32_t u; float f; } v;
        v.u = ((uint32_t)r[i+1] << 16) | r[i];
        printf(" %.3f", (double)v.f);
    }
    printf("\n  float ABCD:");
    for (int i = 0; i + 1 < n; i += 2) {
        union { uint32_t u; float f; } v;
        v.u = ((uint32_t)r[i] << 16) | r[i+1];
        printf(" %.3f", (double)v.f);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: mb_probe COMx [baud slave fc addr n]\n"); return 1; }
    const char *port = argv[1];

    if (argc >= 7) {   /* single explicit read */
        int baud = atoi(argv[2]), slave = atoi(argv[3]), fc = atoi(argv[4]);
        int addr = atoi(argv[5]), n = atoi(argv[6]);
        HANDLE h = ser_open(port, baud);
        if (h == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", port); return 1; }
        uint16_t regs[128];
        int rc = mb_read(h, slave, fc, addr, n, regs);
        if (rc == 0) dump(slave, fc, addr, regs, n);
        else printf("no/bad response (rc=%d)\n", rc);
        CloseHandle(h);
        return 0;
    }

    static const int bauds[] = { 9600, 19200, 38400, 57600, 115200 };
    int found = 0;
    for (int b = 0; b < 5 && !found; b++) {
        HANDLE h = ser_open(port, bauds[b]);
        if (h == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", port); return 1; }
        printf("baud %d...\n", bauds[b]);
        for (int slave = 1; slave <= 16 && !found; slave++) {
            for (int fci = 0; fci < 2 && !found; fci++) {
                int fc = fci == 0 ? 3 : 4;
                uint16_t regs[16];
                int rc = mb_read(h, slave, fc, 0, 16, regs);
                if (rc == 0) {
                    printf("FOUND: baud %d slave %d fc%d addr 0\n",
                           bauds[b], slave, fc);
                    dump(slave, fc, 0, regs, 16);
                    found = 1;
                } else if (rc > 0) {
                    printf("FOUND device: baud %d slave %d fc%d -> exception %d"
                           " (device alive, wrong addr/fc)\n",
                           bauds[b], slave, fc, rc);
                }
            }
        }
        CloseHandle(h);
    }
    if (!found) printf("scan finished - no clean response; card may use"
                       " different addr range, parity or an exception was"
                       " reported above\n");
    return 0;
}
