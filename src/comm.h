/* comm.h - data acquisition thread.
 * Source is selected by g_cfg.source: built-in simulator, or Modbus RTU
 * master polling iAI_U8 cards over RS-485 (COMx / /dev/ttyUSB0). */
#ifndef COMM_H
#define COMM_H

#include <stdint.h>

void comm_init(void);          /* starts the acquisition thread */

/* Modbus link status for the UI ("COMM OK" / "COMM FAIL" indicator) */
int  comm_link_ok(void);

/* ---- structured register access (service / calibration page) ----
 * Safe alongside the poll loop: every bus transaction is serialized.
 * All return 0 on success, <0 on failure. Modbus source only. */
int  mb_service_read(int slave, int fc, int addr, int n, uint16_t *out);
int  mb_service_write(int slave, int addr, uint16_t val);   /* FC06 */

/* read the card's sensor-type registers (FC03 0..7) and update the
 * per-channel sensor names shown in the UI */
void comm_refresh_types(int card);

/* Change the baud rate of the whole RS-485 bus in one action:
 * program every fitted card (HR201 + save key) at the current baud,
 * then reopen the recorder's own port at the new rate and verify each
 * card answers there. Returns the number of cards verified at the new
 * rate, or <0 if nothing could be done (no link / bad rate); `report`
 * receives a per-card summary for the UI either way. The recorder port
 * is left at the new rate, so the caller must store it in g_cfg.baud. */
int comm_bus_baud_change(int new_baud, char *report, int rep_n);

/* mINT-08AI input type code -> display name (codes 0..19) */
const char *mb_type_name(unsigned code);

/* iAI_U8 register addresses (FC03/FC04 zero-based) */
#define REG_PV         0    /* FC04: process values int16, 8 channels */
#define REG_AMBIENT    8    /* FC04: CJC ambient x10 */
#define REG_TYPE       0    /* FC03: input type per channel */
#define REG_DECIMALS   120  /* FC03: decimals for V/I channels */
#define REG_USER_ZERO  72   /* FC03: user zero counts per channel (linear) */
#define REG_USER_SPAN  96   /* FC03: user span counts per channel (linear) */
/* calibration (from iAI_U8 firmware modbus_regs.h) */
#define REG_CAL_VALID  152  /* FC03: cal_valid flag per channel, read only */
#define REG_CAL_SELECT 160  /* write (ch+1)*1000 + type to start, 0 to end */
#define REG_CAL_COUNT  161  /* read: live ADC count / 1000 of cal channel */
#define REG_CAL_LO     162  /* write LOW reference x100  (self-clearing) */
#define REG_CAL_HI     163  /* write HIGH reference x100 (self-clearing) */
#define REG_CAL_OFFSET 165  /* write offset-trim reference x100 */
/* card communication settings (applied by the save key) */
#define REG_COMM_SLAVE 200  /* card's Modbus slave address */
#define REG_COMM_BAUD  201  /* 1=9600 2=19200 3=38400 4=57600 5=115200 */
#define REG_COMM_SAVE  204  /* write 1221: save to NVS + re-init UART */

#endif
