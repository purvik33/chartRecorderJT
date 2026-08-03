/* email.h - alarm email notifications (SMTP via the system curl).
 * Recipients: 5 master addresses (all alarms) + 5 per channel group.
 * Configured from the web dashboard only. */
#ifndef EMAIL_H
#define EMAIL_H

void email_init(void);   /* start the background sender thread */

/* Enqueue a notification for a new alarm. MUST be called with the data
 * lock held (it reads g_ch[ch]); it only copies + queues, never blocks. */
void email_alarm_notify(int ch, int type, float value);

#endif
