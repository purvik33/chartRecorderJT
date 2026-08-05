/* config.h - application settings, persisted to recorder.ini */
#ifndef CONFIG_H
#define CONFIG_H

/* manufacturer master password - accepted at every password/PIN prompt so a
 * device can always be recovered even if the customer changed everything */
#define MASTER_PW "39003900"

#define EMAIL_MASTERS   5   /* master recipients (all alarms) */
#define EMAIL_GROUPS    5   /* one recipient set per channel group */
#define EMAIL_PER_GROUP 5   /* recipients per group */

typedef enum { SRC_SIM = 0, SRC_MODBUS = 1 } data_src_t;

/* 21 CFR user account (see users.h for roles) */
typedef struct {
    char name[17];
    char pin[12];
    int  role;         /* 0 operator, 1 supervisor, 2 administrator */
    int  active;
    int  pin_set;      /* day-number (epoch/86400) the PIN was last set; 0=unknown */
    int  pin_expiry;   /* per-user PIN aging in days; 0 = use global default */
} cfr_user_t;

/* register value encoding on the card */
typedef enum {
    FMT_FLOAT = 0,     /* 2 regs per channel, float32 */
    FMT_I16_10,        /* 1 reg per channel, signed int16 / 10 */
    FMT_I16_100,       /* 1 reg per channel, signed int16 / 100 */
    FMT_I16_RAW        /* 1 reg per channel, signed int16 as-is */
} val_fmt_t;

typedef struct {
    int  source;          /* SRC_SIM or SRC_MODBUS */
    char port[32];        /* COM13 (Windows) or /dev/ttyUSB0 (Pi) */
    int  baud;            /* 9600..115200 */
    int  cards;           /* number of iAI_U8 cards, 1..5 */
    int  slave_base;      /* Modbus slave id of card 1 (card N = base+N-1) */
    int  func;            /* 3 = holding regs, 4 = input regs */
    int  reg_base;        /* first register of channel data */
    int  word_order;      /* float only: 0 = CDAB, 1 = ABCD */
    int  fmt;             /* val_fmt_t */
    int  poll_ms;         /* target time for one full scan of all cards (ms) */
    int  store_interval;  /* logging interval, seconds */
    int  retention_days;  /* keep logs this many days (0 = keep until disk-full) */
    char brand[40];       /* company / product name shown to the customer */
    char model[24];       /* model name shown on splash and About */
    int  theme;           /* product colour theme index */
    int  color_mode;      /* 0 = multi-colour per channel, 1 = single */
    int  single_color;    /* palette index used in single mode */
    int  trend_style;     /* 0 = line (full width), 1 = line + values panel */
    int  ch_color[40];    /* palette index per channel (all cards) */

    /* network / Modbus TCP server */
    int  tcp_enable;      /* 1 = Modbus TCP server on */
    int  tcp_port;        /* TCP port, default 502 */
    int  tcp_unit;        /* unit id of the recorder's own map */
    int  net_dhcp;        /* 1 = DHCP, 0 = static (applied on the Pi) */
    char net_ip[20];      /* static address settings */
    char net_mask[20];
    char net_gw[20];
    char net_dns[20];

    /* Wi-Fi (CM4 wireless variant / Pi 4 onboard) */
    int  wifi_enable;
    char wifi_ssid[33];
    char wifi_pass[65];
    int  wifi_dhcp;
    char wifi_ip[20];
    char wifi_mask[20];
    char wifi_gw[20];

    char factory_pin[12]; /* service password: gates the settings menu */
    char manuf_pin[12];   /* manufacturer password: gates Factory settings (data source / 21 CFR) */

    /* OTA update: GitHub repo "owner/name" + token for private repos */
    char update_repo[64];
    char update_token[84];

    /* built-in web server (read-only dashboard + CSV download) */
    int  web_enable;
    int  web_port;
    int  web_auth;        /* 1 = require login on the web dashboard */
    char web_user[24];    /* web dashboard username */
    char web_pass[40];    /* web dashboard password */

    /* 21 CFR Part 11 mode + user accounts (slot 0 is always the
     * built-in administrator so a lockout is impossible) */
    int        cfr_enable;
    int        esign_enable;     /* print executed e-signature on reports */
    int        pin_expiry_days;  /* PIN aging; 0 = never expires (§11.300) */
    cfr_user_t users[8];

    /* ---- email alarm notifications (web-configured) ---- */
    int  email_enable;
    char smtp_host[64];
    int  smtp_port;              /* 587 STARTTLS / 465 SSL / 25 plain */
    int  smtp_security;          /* 0 none, 1 STARTTLS, 2 SSL/TLS */
    char smtp_user[64];
    char smtp_pass[64];
    char smtp_from[64];
    char email_master[EMAIL_MASTERS][64];             /* alarms on any group */
    char email_group[EMAIL_GROUPS][EMAIL_PER_GROUP][64]; /* per-group */
} app_cfg_t;

extern app_cfg_t g_cfg;

void config_load(void);   /* reads recorder.ini; missing file = defaults */
void config_save(void);   /* writes recorder.ini incl. channel setup */

#endif
