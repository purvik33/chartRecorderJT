#include "config.h"
#include "data_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/stat.h>   /* chmod() - POSIX only */
#endif

#define INI_PATH "recorder.ini"

app_cfg_t g_cfg;

/* serialises the whole of config_save() so the TCP thread and the UI
 * thread cannot interleave writes to recorder.ini */
static pthread_mutex_t cfg_mtx = PTHREAD_MUTEX_INITIALIZER;

static void set_defaults(void)
{
    /* defaults match the iAI_U8 card as probed on the bench:
     * FC04 input regs 0..7, signed int16 / 10, slave 1 @ 9600 */
    g_cfg.source         = SRC_SIM;
    strcpy(g_cfg.port, "COM13");
    g_cfg.baud           = 9600;
    g_cfg.cards          = 1;
    g_cfg.slave_base     = 1;
    g_cfg.func           = 4;
    g_cfg.reg_base       = 0;
    g_cfg.word_order     = 0;
    g_cfg.fmt            = FMT_I16_10;
    g_cfg.store_interval = 5;
    strcpy(g_cfg.brand, "JETPACE");
    strcpy(g_cfg.model, "PR-40 Recorder");
    g_cfg.theme        = 0;
    g_cfg.color_mode   = 0;
    g_cfg.single_color = 0;
    for (int i = 0; i < 40; i++) g_cfg.ch_color[i] = i % 8;

    g_cfg.tcp_enable = 1;
    g_cfg.tcp_port   = 502;
    g_cfg.tcp_unit   = 1;
    g_cfg.net_dhcp   = 1;
    strcpy(g_cfg.net_ip,   "192.168.1.100");
    strcpy(g_cfg.net_mask, "255.255.255.0");
    strcpy(g_cfg.net_gw,   "192.168.1.1");
    strcpy(g_cfg.net_dns,  "8.8.8.8");

    g_cfg.wifi_enable = 0;
    g_cfg.wifi_ssid[0] = 0;
    g_cfg.wifi_pass[0] = 0;
    g_cfg.wifi_dhcp = 1;
    strcpy(g_cfg.wifi_ip,   "192.168.1.101");
    strcpy(g_cfg.wifi_mask, "255.255.255.0");
    strcpy(g_cfg.wifi_gw,   "192.168.1.1");
    strcpy(g_cfg.factory_pin, "1234");
    g_cfg.update_repo[0]  = 0;
    g_cfg.update_token[0] = 0;
    g_cfg.web_enable = 1;
    g_cfg.web_port   = 8080;

    g_cfg.cfr_enable = 0;
    memset(g_cfg.users, 0, sizeof(g_cfg.users));
    strcpy(g_cfg.users[0].name, "SUPER ADMIN");
    strcpy(g_cfg.users[0].pin,  "1234");
    g_cfg.users[0].role   = 3;   /* super admin */
    g_cfg.users[0].active = 1;
}

void config_load(void)
{
    set_defaults();

    FILE *f = fopen(INI_PATH, "r");
    if (!f) return;

    char line[128];
    int ch = -1;
    while (fgets(line, sizeof(line), f)) {
        char key[32], val[64];
        if (line[0] == '[') {
            ch = -1;
            if (sscanf(line, "[ch%d]", &ch) == 1) ch -= 1;
            continue;
        }
        if (sscanf(line, "%31[^=]=%63[^\n]", key, val) != 2) continue;

        if (ch >= 0 && ch < CH_TOTAL) {
            channel_t *c = &g_ch[ch];
            if      (!strcmp(key, "tag"))    { strncpy(c->tag,  val, sizeof(c->tag)-1);  c->tag[sizeof(c->tag)-1] = 0; }
            else if (!strcmp(key, "unit"))   { strncpy(c->unit, val, sizeof(c->unit)-1); c->unit[sizeof(c->unit)-1] = 0; }
            else if (!strcmp(key, "lo"))     c->lo     = (float)atof(val);
            else if (!strcmp(key, "hi"))     c->hi     = (float)atof(val);
            else if (!strcmp(key, "alm_hi")) c->alm_hi = (float)atof(val);
            else if (!strcmp(key, "alm_lo")) c->alm_lo = (float)atof(val);
        } else {
            if      (!strcmp(key, "source"))         g_cfg.source = atoi(val);
            else if (!strcmp(key, "port"))           { strncpy(g_cfg.port, val, sizeof(g_cfg.port)-1); g_cfg.port[sizeof(g_cfg.port)-1] = 0; }
            else if (!strcmp(key, "baud"))           g_cfg.baud = atoi(val);
            else if (!strcmp(key, "cards"))          g_cfg.cards = atoi(val);
            else if (!strcmp(key, "slave_base"))     g_cfg.slave_base = atoi(val);
            else if (!strcmp(key, "func"))           g_cfg.func = atoi(val);
            else if (!strcmp(key, "reg_base"))       g_cfg.reg_base = atoi(val);
            else if (!strcmp(key, "word_order"))     g_cfg.word_order = atoi(val);
            else if (!strcmp(key, "fmt"))            g_cfg.fmt = atoi(val);
            else if (!strcmp(key, "store_interval")) g_cfg.store_interval = atoi(val);
            else if (!strcmp(key, "brand"))          { strncpy(g_cfg.brand, val, sizeof(g_cfg.brand)-1); g_cfg.brand[sizeof(g_cfg.brand)-1] = 0; }
            else if (!strcmp(key, "model"))          { strncpy(g_cfg.model, val, sizeof(g_cfg.model)-1); g_cfg.model[sizeof(g_cfg.model)-1] = 0; }
            else if (!strcmp(key, "theme"))          g_cfg.theme = atoi(val);
            else if (!strcmp(key, "color_mode"))     g_cfg.color_mode = atoi(val);
            else if (!strcmp(key, "single_color"))   g_cfg.single_color = atoi(val);
            else if (!strcmp(key, "tcp_enable"))     g_cfg.tcp_enable = atoi(val);
            else if (!strcmp(key, "tcp_port"))       g_cfg.tcp_port = atoi(val);
            else if (!strcmp(key, "tcp_unit"))       g_cfg.tcp_unit = atoi(val);
            else if (!strcmp(key, "net_dhcp"))       g_cfg.net_dhcp = atoi(val);
            else if (!strcmp(key, "net_ip"))         { strncpy(g_cfg.net_ip,   val, sizeof(g_cfg.net_ip)-1);   g_cfg.net_ip[sizeof(g_cfg.net_ip)-1] = 0; }
            else if (!strcmp(key, "net_mask"))       { strncpy(g_cfg.net_mask, val, sizeof(g_cfg.net_mask)-1); g_cfg.net_mask[sizeof(g_cfg.net_mask)-1] = 0; }
            else if (!strcmp(key, "net_gw"))         { strncpy(g_cfg.net_gw,   val, sizeof(g_cfg.net_gw)-1);   g_cfg.net_gw[sizeof(g_cfg.net_gw)-1] = 0; }
            else if (!strcmp(key, "net_dns"))        { strncpy(g_cfg.net_dns,  val, sizeof(g_cfg.net_dns)-1);  g_cfg.net_dns[sizeof(g_cfg.net_dns)-1] = 0; }
            else if (!strcmp(key, "wifi_enable"))    g_cfg.wifi_enable = atoi(val);
            else if (!strcmp(key, "wifi_ssid"))      { strncpy(g_cfg.wifi_ssid, val, sizeof(g_cfg.wifi_ssid)-1); g_cfg.wifi_ssid[sizeof(g_cfg.wifi_ssid)-1] = 0; }
            else if (!strcmp(key, "wifi_pass"))      { strncpy(g_cfg.wifi_pass, val, sizeof(g_cfg.wifi_pass)-1); g_cfg.wifi_pass[sizeof(g_cfg.wifi_pass)-1] = 0; }
            else if (!strcmp(key, "wifi_dhcp"))      g_cfg.wifi_dhcp = atoi(val);
            else if (!strcmp(key, "wifi_ip"))        { strncpy(g_cfg.wifi_ip,   val, sizeof(g_cfg.wifi_ip)-1);   g_cfg.wifi_ip[sizeof(g_cfg.wifi_ip)-1] = 0; }
            else if (!strcmp(key, "wifi_mask"))      { strncpy(g_cfg.wifi_mask, val, sizeof(g_cfg.wifi_mask)-1); g_cfg.wifi_mask[sizeof(g_cfg.wifi_mask)-1] = 0; }
            else if (!strcmp(key, "wifi_gw"))        { strncpy(g_cfg.wifi_gw,   val, sizeof(g_cfg.wifi_gw)-1);   g_cfg.wifi_gw[sizeof(g_cfg.wifi_gw)-1] = 0; }
            else if (!strcmp(key, "factory_pin"))    { strncpy(g_cfg.factory_pin, val, sizeof(g_cfg.factory_pin)-1); g_cfg.factory_pin[sizeof(g_cfg.factory_pin)-1] = 0; }
            else if (!strcmp(key, "web_enable"))     g_cfg.web_enable = atoi(val);
            else if (!strcmp(key, "web_port"))       g_cfg.web_port = atoi(val);
            else if (!strcmp(key, "update_repo"))    { strncpy(g_cfg.update_repo,  val, sizeof(g_cfg.update_repo)-1);  g_cfg.update_repo[sizeof(g_cfg.update_repo)-1] = 0; }
            else if (!strcmp(key, "update_token"))   { strncpy(g_cfg.update_token, val, sizeof(g_cfg.update_token)-1); g_cfg.update_token[sizeof(g_cfg.update_token)-1] = 0; }
            else if (!strcmp(key, "cfr_enable"))     g_cfg.cfr_enable = atoi(val);
            else if (!strncmp(key, "u", 1) && key[1] >= '1' && key[1] <= '8'
                     && key[2] == '_') {
                cfr_user_t *u = &g_cfg.users[key[1] - '1'];
                if      (!strcmp(key + 3, "name"))   { strncpy(u->name, val, sizeof(u->name)-1); u->name[sizeof(u->name)-1] = 0; }
                else if (!strcmp(key + 3, "pin"))    { strncpy(u->pin,  val, sizeof(u->pin)-1);  u->pin[sizeof(u->pin)-1] = 0; }
                else if (!strcmp(key + 3, "role"))   u->role = atoi(val);
                else if (!strcmp(key + 3, "active")) u->active = atoi(val);
            }
            else if (!strncmp(key, "ch_color", 8)) {
                int i = atoi(key + 8);
                if (i >= 0 && i < 40) g_cfg.ch_color[i] = atoi(val);
            }
        }
    }
    fclose(f);

    /* Validate / clamp everything that could index arrays or drive I/O
     * later. A bad recorder.ini (hand-edited or corrupt) must never be
     * able to push us out of bounds - clamp defensively, don't crash.
     * cards is the critical one: the poll loop indexes g_ch[card*8+c]
     * against CH_TOTAL (40), so cards must stay in 1..5. */
    if (g_cfg.cards < 1) g_cfg.cards = 1;
    if (g_cfg.cards > GROUP_COUNT) g_cfg.cards = GROUP_COUNT;

    if (g_cfg.slave_base < 1) g_cfg.slave_base = 1;

    if (g_cfg.func != 3 && g_cfg.func != 4) g_cfg.func = 4;

    if (g_cfg.fmt < FMT_FLOAT || g_cfg.fmt > FMT_I16_RAW) g_cfg.fmt = FMT_I16_10;

    if (g_cfg.word_order != 0 && g_cfg.word_order != 1) g_cfg.word_order = 0;

    if (g_cfg.tcp_port < 1 || g_cfg.tcp_port > 65535) g_cfg.tcp_port = 502;
    if (g_cfg.web_port < 1 || g_cfg.web_port > 65535) g_cfg.web_port = 8080;

    /* minimum store interval is 1 minute (older configs may be lower) */
    if (g_cfg.store_interval < 60) g_cfg.store_interval = 60;

    /* slot 1 is the permanent super admin - enforce it even if the
     * file was edited by hand or written by an older version (the old
     * default name "ADMIN" migrates to "SUPER ADMIN") */
    g_cfg.users[0].role   = 3;
    g_cfg.users[0].active = 1;
    if (!g_cfg.users[0].name[0] || !strcmp(g_cfg.users[0].name, "ADMIN"))
        strcpy(g_cfg.users[0].name, "SUPER ADMIN");
    if (!g_cfg.users[0].pin[0])  strcpy(g_cfg.users[0].pin,  "1234");
}

void config_save(void)
{
    /* one writer at a time - two concurrent savers would interleave
     * fprintf output and corrupt recorder.ini */
    pthread_mutex_lock(&cfg_mtx);

    FILE *f = fopen(INI_PATH, "w");
    if (!f) { pthread_mutex_unlock(&cfg_mtx); return; }

    fprintf(f, "[comm]\n");
    fprintf(f, "source=%d\n",     g_cfg.source);
    fprintf(f, "port=%s\n",       g_cfg.port);
    fprintf(f, "baud=%d\n",       g_cfg.baud);
    fprintf(f, "cards=%d\n",      g_cfg.cards);
    fprintf(f, "slave_base=%d\n", g_cfg.slave_base);
    fprintf(f, "func=%d\n",       g_cfg.func);
    fprintf(f, "reg_base=%d\n",   g_cfg.reg_base);
    fprintf(f, "word_order=%d\n", g_cfg.word_order);
    fprintf(f, "fmt=%d\n",        g_cfg.fmt);
    fprintf(f, "\n[logging]\n");
    fprintf(f, "store_interval=%d\n", g_cfg.store_interval);
    fprintf(f, "\n[network]\n");
    fprintf(f, "tcp_enable=%d\n", g_cfg.tcp_enable);
    fprintf(f, "tcp_port=%d\n",   g_cfg.tcp_port);
    fprintf(f, "tcp_unit=%d\n",   g_cfg.tcp_unit);
    fprintf(f, "net_dhcp=%d\n",   g_cfg.net_dhcp);
    fprintf(f, "net_ip=%s\n",     g_cfg.net_ip);
    fprintf(f, "net_mask=%s\n",   g_cfg.net_mask);
    fprintf(f, "net_gw=%s\n",     g_cfg.net_gw);
    fprintf(f, "net_dns=%s\n",    g_cfg.net_dns);
    fprintf(f, "web_enable=%d\n", g_cfg.web_enable);
    fprintf(f, "web_port=%d\n",   g_cfg.web_port);
    fprintf(f, "wifi_enable=%d\n", g_cfg.wifi_enable);
    fprintf(f, "wifi_ssid=%s\n",   g_cfg.wifi_ssid);
    fprintf(f, "wifi_pass=%s\n",   g_cfg.wifi_pass);
    fprintf(f, "wifi_dhcp=%d\n",   g_cfg.wifi_dhcp);
    fprintf(f, "wifi_ip=%s\n",     g_cfg.wifi_ip);
    fprintf(f, "wifi_mask=%s\n",   g_cfg.wifi_mask);
    fprintf(f, "wifi_gw=%s\n",     g_cfg.wifi_gw);
    fprintf(f, "factory_pin=%s\n", g_cfg.factory_pin);
    fprintf(f, "\n[update]\n");
    fprintf(f, "update_repo=%s\n",  g_cfg.update_repo);
    fprintf(f, "update_token=%s\n", g_cfg.update_token);
    fprintf(f, "\n[users]\n");
    fprintf(f, "cfr_enable=%d\n", g_cfg.cfr_enable);
    for (int i = 0; i < 8; i++) {
        cfr_user_t *u = &g_cfg.users[i];
        if (!u->name[0]) continue;
        fprintf(f, "u%d_name=%s\nu%d_pin=%s\nu%d_role=%d\nu%d_active=%d\n",
                i + 1, u->name, i + 1, u->pin, i + 1, u->role,
                i + 1, u->active);
    }
    fprintf(f, "\n[system]\n");
    fprintf(f, "brand=%s\n", g_cfg.brand);
    fprintf(f, "model=%s\n", g_cfg.model);
    fprintf(f, "theme=%d\n",        g_cfg.theme);
    fprintf(f, "color_mode=%d\n",   g_cfg.color_mode);
    fprintf(f, "single_color=%d\n", g_cfg.single_color);
    for (int i = 0; i < 40; i++)
        fprintf(f, "ch_color%d=%d\n", i, g_cfg.ch_color[i]);

    /* g_ch[] is shared with the comm/UI threads - read it under the
     * data lock so a concurrent update can't be captured mid-write */
    data_lock();
    for (int i = 0; i < CH_TOTAL; i++) {
        channel_t *c = &g_ch[i];
        fprintf(f, "\n[ch%d]\n", i + 1);
        fprintf(f, "tag=%s\nunit=%s\nlo=%g\nhi=%g\nalm_hi=%g\nalm_lo=%g\n",
                c->tag, c->unit, (double)c->lo, (double)c->hi,
                (double)c->alm_hi, (double)c->alm_lo);
    }
    data_unlock();

    fclose(f);

    /* recorder.ini holds wifi_pass, PINs and the OTA token - keep it
     * readable by the owner only (POSIX; no-op on the Windows sim) */
#ifndef _WIN32
    chmod(INI_PATH, 0600);
#endif

    pthread_mutex_unlock(&cfg_mtx);
}
