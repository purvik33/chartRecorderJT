/* modbus_tcp.h - Modbus TCP server + transparent RS-485 gateway.
 *
 * Unit id routing:
 *   g_cfg.tcp_unit (default 1) = the recorder's own register map
 *   11..15                     = direct gateway to AI card 1..5:
 *                                the PDU is forwarded over RS-485 to
 *                                slave (slave_base + N - 1), so every
 *                                card parameter readable/writable on
 *                                the RTU bus is also available on TCP.
 *
 * Recorder map (unit id tcp_unit):
 *   Input registers (FC04):
 *     0..39    PV x10, int16 (abnormal codes 32764..32767 passed on)
 *     100..179 PV float32, 2 regs/channel, word order per config
 *     200..239 channel status: 0 OK, 1 ALM HI, 2 ALM LO, 3 SKIP,
 *              4 UNDER, 5 OVER, 6 OPEN, 7 COMM
 *     240 RS-485 link (0/1), 241 active alarms, 242 unack alarms,
 *     243 store interval s, 244 cards fitted
 *   Holding registers (FC03, writable FC06/FC16):
 *     0..39    alarm HIGH setpoint x10 per channel
 *     40..79   alarm LOW  setpoint x10 per channel
 *     100      store interval, seconds (60..3600)
 *     101      write 1221 = acknowledge all alarms (reads 0)
 */
#ifndef MODBUS_TCP_H
#define MODBUS_TCP_H

void modbus_tcp_init(void);      /* starts the server thread */
int  modbus_tcp_clients(void);   /* connected client count (for the UI) */

/* current IPv4 address of the box, "-" when no network */
void net_current_ip(char *buf, int n);

/* apply g_cfg static/DHCP settings to the OS (Pi only; sim = no-op) */
void net_apply(void);

/* apply g_cfg Wi-Fi settings (connect / disconnect) - Pi only */
void wifi_apply(void);

/* scan for visible Wi-Fi networks (blocking, a few seconds - call
 * from a worker thread); fills `out` with up to `max` unique SSIDs,
 * returns the count. Pi: nmcli; Windows sim: netsh. */
int wifi_scan(char (*out)[33], int max);

#endif
