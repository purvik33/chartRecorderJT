/* email.c - alarm email notifications over SMTP.
 * Uses the system `curl` (same approach as update.c). The password is
 * written to a temp curl config file (-K), never onto the command line.
 * A small queue + one worker thread keeps sending off the alarm thread. */
#include "email.h"
#include "config.h"
#include "data_model.h"
#include "alarm.h"
#include "events.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
/* the Windows build is only the desktop simulator - no mail */
void email_init(void) {}
void email_alarm_notify(int ch, int type, float value)
{ (void)ch; (void)type; (void)value; }
int email_send_test(void) { return 2; }
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>

#define EQ 16
typedef struct {
    int   ch, group, type;
    float value;
    time_t t;
    char  tag[16], unit[12];
} enote_t;

static enote_t q[EQ];
static int q_head, q_tail;
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;

static const char *atype_txt(int t)
{
    switch (t) {
    case ALM_HI:   return "HIGH alarm";
    case ALM_LO:   return "LOW alarm";
    case ALM_COMM: return "Communication fault";
    case ALM_OPEN: return "Sensor / open-circuit fault";
    default:       return "Alarm";
    }
}

/* caller holds the data lock (push_event) - safe to read g_ch here */
void email_alarm_notify(int ch, int type, float value)
{
    if (!g_cfg.email_enable) return;
    if (ch < 0 || ch >= CH_TOTAL) return;

    pthread_mutex_lock(&q_mtx);
    int nxt = (q_head + 1) % EQ;
    if (nxt != q_tail) {                 /* room in the ring */
        enote_t *e = &q[q_head];
        e->ch = ch; e->group = ch / CH_PER_GROUP; e->type = type;
        e->value = value; e->t = time(NULL);
        strncpy(e->tag,  g_ch[ch].tag,  sizeof(e->tag) - 1);  e->tag[sizeof(e->tag)-1]  = 0;
        strncpy(e->unit, g_ch[ch].unit, sizeof(e->unit) - 1); e->unit[sizeof(e->unit)-1] = 0;
        q_head = nxt;
    }
    pthread_mutex_unlock(&q_mtx);
}

/* write a value into a curl -K config file as a quoted string */
static void cfg_esc(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\r' || *s == '\n') continue;
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
}

static void send_one(const enote_t *e)
{
    if (!g_cfg.email_enable || !g_cfg.smtp_host[0]) return;

    const char *rcpt[EMAIL_MASTERS + EMAIL_PER_GROUP];
    int nr = 0;
    for (int i = 0; i < EMAIL_MASTERS; i++)
        if (g_cfg.email_master[i][0]) rcpt[nr++] = g_cfg.email_master[i];
    if (e->group >= 0 && e->group < EMAIL_GROUPS)
        for (int i = 0; i < EMAIL_PER_GROUP; i++)
            if (g_cfg.email_group[e->group][i][0]) rcpt[nr++] = g_cfg.email_group[e->group][i];
    if (nr == 0) return;

    const char *from = g_cfg.smtp_from[0] ? g_cfg.smtp_from : g_cfg.smtp_user;
    if (!from[0]) return;

    struct tm tm; time_t t = e->t; localtime_r(&t, &tm);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    FILE *m = fopen("/tmp/pr40_mail.txt", "w");
    if (!m) return;
    fprintf(m, "From: %s\r\n", from);
    fprintf(m, "To: ");
    for (int i = 0; i < nr; i++) fprintf(m, "%s%s", i ? ", " : "", rcpt[i]);
    fprintf(m, "\r\n");
    fprintf(m, "Subject: [ALARM] CH%d %s - %s\r\n", e->ch+1, e->tag, atype_txt(e->type));
    fprintf(m, "Content-Type: text/plain; charset=UTF-8\r\n\r\n");
    fprintf(m, "An alarm was raised on the JETPACE PR-40 recorder.\r\n\r\n");
    fprintf(m, "  Channel : CH%d  %s\r\n", e->ch+1, e->tag);
    fprintf(m, "  Event   : %s\r\n", atype_txt(e->type));
    if (e->type == ALM_HI || e->type == ALM_LO)
        fprintf(m, "  Value   : %.2f %s\r\n", (double)e->value, e->unit);
    fprintf(m, "  Group   : %d\r\n", e->group+1);
    fprintf(m, "  Time    : %s\r\n\r\n", ts);
    fprintf(m, "-- \r\nAutomated notification - please do not reply.\r\n");
    fclose(m);

    FILE *c = fopen("/tmp/pr40_mail.cfg", "w");
    if (!c) { remove("/tmp/pr40_mail.txt"); return; }
    fprintf(c, "url = \"smtp%s://", g_cfg.smtp_security == 2 ? "s" : "");
    cfg_esc(c, g_cfg.smtp_host);
    fprintf(c, ":%d\"\n", g_cfg.smtp_port);
    if (g_cfg.smtp_security >= 1) fprintf(c, "ssl-reqd\n");
    if (g_cfg.smtp_user[0]) {
        fprintf(c, "user = \"");
        cfg_esc(c, g_cfg.smtp_user); fputc(':', c); cfg_esc(c, g_cfg.smtp_pass);
        fprintf(c, "\"\n");
    }
    fprintf(c, "mail-from = \""); cfg_esc(c, from); fprintf(c, "\"\n");
    for (int i = 0; i < nr; i++) {
        fprintf(c, "mail-rcpt = \""); cfg_esc(c, rcpt[i]); fprintf(c, "\"\n");
    }
    fprintf(c, "upload-file = \"/tmp/pr40_mail.txt\"\n");
    fclose(c);
    chmod("/tmp/pr40_mail.cfg", 0600);

    int rc = system("curl -s --max-time 25 -K /tmp/pr40_mail.cfg >/dev/null 2>&1");
    event_log("EMAIL", rc == 0 ? "Alarm email sent for CH%d %s (%d recipients)"
                                : "Alarm email FAILED for CH%d %s (%d recipients)",
              e->ch + 1, e->tag, nr);
    remove("/tmp/pr40_mail.cfg");
    remove("/tmp/pr40_mail.txt");
}

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(1500 * 1000);
        for (;;) {
            enote_t e;
            pthread_mutex_lock(&q_mtx);
            if (q_tail == q_head) { pthread_mutex_unlock(&q_mtx); break; }
            e = q[q_tail]; q_tail = (q_tail + 1) % EQ;
            pthread_mutex_unlock(&q_mtx);
            send_one(&e);
        }
    }
    return NULL;
}

int email_send_test(void)
{
    if (!g_cfg.smtp_host[0]) return 2;
    const char *from = g_cfg.smtp_from[0] ? g_cfg.smtp_from : g_cfg.smtp_user;
    if (!from[0]) return 2;

    const char *rcpt[EMAIL_MASTERS + 1];
    int nr = 0;
    for (int i = 0; i < EMAIL_MASTERS; i++)
        if (g_cfg.email_master[i][0]) rcpt[nr++] = g_cfg.email_master[i];
    if (nr == 0) rcpt[nr++] = from;     /* self-test to the From address */

    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    FILE *m = fopen("/tmp/pr40_test.txt", "w");
    if (!m) return 1;
    fprintf(m, "From: %s\r\n", from);
    fprintf(m, "To: ");
    for (int i = 0; i < nr; i++) fprintf(m, "%s%s", i ? ", " : "", rcpt[i]);
    fprintf(m, "\r\n");
    fprintf(m, "Subject: JETPACE PR-40 - test email\r\n");
    fprintf(m, "Content-Type: text/plain; charset=UTF-8\r\n\r\n");
    fprintf(m, "This is a test message from the JETPACE PR-40 recorder.\r\n\r\n");
    fprintf(m, "If you received this, alarm email notifications are set up correctly.\r\n");
    fprintf(m, "Sent: %s\r\n", ts);
    fclose(m);

    FILE *c = fopen("/tmp/pr40_test.cfg", "w");
    if (!c) { remove("/tmp/pr40_test.txt"); return 1; }
    fprintf(c, "url = \"smtp%s://", g_cfg.smtp_security == 2 ? "s" : "");
    cfg_esc(c, g_cfg.smtp_host);
    fprintf(c, ":%d\"\n", g_cfg.smtp_port);
    if (g_cfg.smtp_security >= 1) fprintf(c, "ssl-reqd\n");
    if (g_cfg.smtp_user[0]) {
        fprintf(c, "user = \"");
        cfg_esc(c, g_cfg.smtp_user); fputc(':', c); cfg_esc(c, g_cfg.smtp_pass);
        fprintf(c, "\"\n");
    }
    fprintf(c, "mail-from = \""); cfg_esc(c, from); fprintf(c, "\"\n");
    for (int i = 0; i < nr; i++) {
        fprintf(c, "mail-rcpt = \""); cfg_esc(c, rcpt[i]); fprintf(c, "\"\n");
    }
    fprintf(c, "upload-file = \"/tmp/pr40_test.txt\"\n");
    fclose(c);
    chmod("/tmp/pr40_test.cfg", 0600);

    int rc = system("curl -s --max-time 20 -K /tmp/pr40_test.cfg >/dev/null 2>&1");
    event_log("EMAIL", rc == 0 ? "Test email sent (%d recipient%s)"
                               : "Test email FAILED (%d recipient%s)",
              nr, nr == 1 ? "" : "s");
    remove("/tmp/pr40_test.cfg");
    remove("/tmp/pr40_test.txt");
    return rc == 0 ? 0 : 1;
}

void email_init(void)
{
    pthread_t t;
    pthread_create(&t, NULL, worker, NULL);
}
#endif
