#include "users.h"
#include "config.h"
#include "events.h"
#include <string.h>

static int logged = -1;

int cfr_on(void) { return g_cfg.cfr_enable; }

int cfr_logged_idx(void) { return logged; }

int cfr_role(void)
{
    return (logged >= 0) ? g_cfg.users[logged].role : -1;
}

const char *cfr_user_name(void)
{
    return (logged >= 0) ? g_cfg.users[logged].name : "SYSTEM";
}

int cfr_login(int idx, const char *pin)
{
    static const char *role_txt[] =
        { "Operator", "Supervisor", "Admin", "Super admin" };

    if (idx < 0 || idx >= CFR_USERS) return -1;
    cfr_user_t *u = &g_cfg.users[idx];
    if (!u->active || strcmp(u->pin, pin) != 0) {
        event_log("SYSTEM", "Login failed for user %s", u->name);
        return -1;
    }
    logged = idx;
    event_log("SYSTEM", "User %s logged in (%s)", u->name,
              role_txt[u->role >= 0 && u->role <= 3 ? u->role : 0]);
    return 0;
}

void cfr_logout(void)
{
    if (logged < 0) return;
    event_log("SYSTEM", "User %s logged out", g_cfg.users[logged].name);
    logged = -1;
}
