#include "users.h"
#include "config.h"
#include "events.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static int logged = -1;

/* §11.300 password controls: lock an account for 5 minutes after 5
 * consecutive failed logins (in-memory, cleared on success/restart) */
#define CFR_MAX_FAILS   5
#define CFR_LOCK_SECS   300
static int    fail_count[CFR_USERS];
static time_t lock_until[CFR_USERS];

int cfr_pin_ok(const char *pin, char *msg, size_t n)
{
    size_t L = strlen(pin);
    if (L < 6) { snprintf(msg, n, "PIN must be at least 6 characters"); return 0; }
    int same = 1, seq = 1;
    for (size_t i = 0; i < L; i++) {
        if (pin[i] != pin[0]) same = 0;
        if (i && pin[i] != pin[i-1] + 1) seq = 0;
    }
    if (same) { snprintf(msg, n, "PIN cannot be all the same character"); return 0; }
    if (seq)  { snprintf(msg, n, "PIN cannot be a simple sequence"); return 0; }
    snprintf(msg, n, "OK"); return 1;
}

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
    time_t now = time(NULL);

    if (now < lock_until[idx]) {
        event_log("SYSTEM", "Login blocked - user %s is locked out", u->name);
        return -1;
    }
    if (!u->active || strcmp(u->pin, pin) != 0) {
        if (u->active && ++fail_count[idx] >= CFR_MAX_FAILS) {
            lock_until[idx] = now + CFR_LOCK_SECS;
            fail_count[idx] = 0;
            event_log("SYSTEM", "User %s locked out for %d min (%d failed"
                      " logins)", u->name, CFR_LOCK_SECS / 60, CFR_MAX_FAILS);
        } else {
            event_log("SYSTEM", "Login failed for user %s", u->name);
        }
        return -1;
    }
    fail_count[idx] = 0; lock_until[idx] = 0;

    /* §11.300 PIN aging: a correct but expired PIN is rejected (the
     * super admin may still enter, so a site can never lock itself out) */
    if (g_cfg.cfr_enable && g_cfg.pin_expiry_days > 0 && u->pin_set > 0 &&
        (now / 86400 - u->pin_set) > g_cfg.pin_expiry_days) {
        event_log("SYSTEM", "PIN expired for user %s (%d-day policy)",
                  u->name, g_cfg.pin_expiry_days);
        if (idx != 0) return -2;
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
