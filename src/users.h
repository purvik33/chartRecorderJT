/* users.h - 21 CFR Part 11 mode: multi-level user accounts.
 * When enabled (Factory settings > Account manager), the Menu requires
 * a login; access is gated by role and every operator/config action in
 * the event trail carries the logged-in user's name. */
#ifndef USERS_H
#define USERS_H

#include <stddef.h>

/* access levels, lowest to highest:
 * Operator    - view, acknowledge, export
 * Supervisor  - + channel / logging / network setup
 * Admin       - + Account manager
 * Super admin - + Factory settings */
enum { ROLE_OPERATOR = 0, ROLE_SUPERVISOR = 1, ROLE_ADMIN = 2,
       ROLE_SUPERADMIN = 3 };

#define CFR_USERS 8

int  cfr_on(void);                       /* 21 CFR mode enabled?      */
int  cfr_login(int idx, const char *pin);/* 0 = ok, -1 = wrong pin    */
void cfr_logout(void);                   /* also called on menu leave */
int  cfr_logged_idx(void);               /* -1 = nobody logged in     */
int  cfr_role(void);                     /* role of logged user, -1   */
const char *cfr_user_name(void);         /* logged name or "SYSTEM"   */

/* validate a new PIN against the §11.300 complexity policy; returns 1 if
 * acceptable, 0 with a reason in msg. */
int cfr_pin_ok(const char *pin, char *msg, size_t n);

#endif
