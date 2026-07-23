/* update.c - OTA update via GitHub Releases, using the system curl
 * (preinstalled on Raspberry Pi OS and Windows 10+). All network work
 * happens in worker threads with bounded curl timeouts, so the UI
 * never blocks. The previous binary is kept beside the new one as
 * .bak for manual rollback. */
#include "update.h"
#include "config.h"
#include "events.h"
#include "version.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#define popen_c  _popen
#define pclose_c _pclose
#define UPD_ASSET "recorder_ui.exe"
#else
#include <unistd.h>
#include <sys/stat.h>
#define popen_c  popen
#define pclose_c pclose
#define UPD_ASSET "recorder_ui"
#endif

static volatile upd_state_t st = UPD_IDLE;
static char msg[200] = "";
static char latest_tag[40];     /* raw tag of the newest release   */
static char asset_url[240];     /* API url of the matching asset -
                                 * works for private repos too     */

upd_state_t update_state(void)   { return st; }
const char *update_message(void) { return msg; }

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
}

/* run a command, capture stdout (bounded) */
static int run_capture(const char *cmd, char *out, int n)
{
    FILE *p = popen_c(cmd, "r");
    if (!p) return -1;
    int got = (int)fread(out, 1, (size_t)(n - 1), p);
    out[got < 0 ? 0 : got] = 0;
    pclose_c(p);
    return got > 0 ? 0 : -1;
}

/* "v1.2.3" / "1.2.3" -> comparable number; -1 if unparseable */
static long ver_num(const char *s)
{
    if (*s == 'v' || *s == 'V') s++;
    int a = 0, b = 0, c = 0;
    if (sscanf(s, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (long)a * 1000000L + b * 1000L + c;
}

/* ---- input hardening for shell command construction ----------------
 * update_repo comes from recorder.ini and latest_tag / asset_url come
 * straight out of the GitHub JSON response; all three end up on a curl
 * command line handed to system()/popen(), so each is validated before
 * use to stop shell-metacharacter injection. */

/* strict version tag: optional v/V then 1-4 dot-separated decimal
 * groups, e.g. "v1.2.3" (matches ^v?[0-9]+(\.[0-9]+){0,3}$) */
static int valid_tag(const char *s)
{
    if (!s || !*s) return 0;
    if (*s == 'v' || *s == 'V') s++;
    int groups = 1, digits = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { digits++; continue; }
        if (*s == '.') {
            if (digits == 0 || ++groups > 4) return 0;
            digits = 0;
            continue;
        }
        return 0;
    }
    return digits > 0;
}

/* owner/repo: letters, digits, - _ . and exactly one '/' separator */
static int valid_repo(const char *s)
{
    if (!s || !*s) return 0;
    int slashes = 0;
    for (; *s; s++) {
        char c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            continue;
        if (c == '/') { if (++slashes > 1) return 0; continue; }
        return 0;
    }
    return slashes == 1;
}

/* asset_url is double-quoted in the command; reject controls, spaces and
 * the metacharacters that could break out of the quotes or the shell */
static int url_safe(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7f) return 0;
        if (strchr("\"'`$\\ ;|&<>(){}*?", c)) return 0;
    }
    return 1;
}

static void auth_arg(char *buf, int n)
{
    if (g_cfg.update_token[0])
        snprintf(buf, (size_t)n, "-H \"Authorization: Bearer %s\" ",
                 g_cfg.update_token);
    else
        buf[0] = 0;
}

/* find the API url of the asset named UPD_ASSET in the release JSON
 * (each asset object carries "url":".../releases/assets/ID" followed
 * by its "name") - downloading via that url with Accept:
 * application/octet-stream works for public AND private repos */
static void find_asset_url(const char *body)
{
    asset_url[0] = 0;
    const char *p = body;
    while ((p = strstr(p, "\"url\":\"")) != NULL) {
        p += 7;
        const char *e = strchr(p, '\"');
        if (!e) return;
        const char *marker = strstr(p, "/releases/assets/");
        if (marker && marker < e) {
            const char *nm = strstr(e, "\"name\":\"");
            if (nm) {
                nm += 8;
                size_t al = strlen(UPD_ASSET);
                if (!strncmp(nm, UPD_ASSET, al) && nm[al] == '\"') {
                    size_t len = (size_t)(e - p);
                    if (len >= sizeof(asset_url))
                        len = sizeof(asset_url) - 1;
                    memcpy(asset_url, p, len);
                    asset_url[len] = 0;
                    return;
                }
            }
        }
        p = e;
    }
}

static void *check_thread(void *a)
{
    (void)a;
    char auth[120], cmd[360];
    static char body[32768];

    if (!valid_repo(g_cfg.update_repo)) {
        set_msg("Update repo '%s' has invalid characters", g_cfg.update_repo);
        st = UPD_ERROR;
        return NULL;
    }

    auth_arg(auth, sizeof(auth));
    snprintf(cmd, sizeof(cmd),
             "curl -sL -m 15 %s"
             "https://api.github.com/repos/%s/releases/latest",
             auth, g_cfg.update_repo);

    if (run_capture(cmd, body, sizeof(body)) != 0) {
        set_msg("Update server not reachable - check the network");
        st = UPD_ERROR;
        return NULL;
    }

    char *t = strstr(body, "\"tag_name\"");
    if (!t || !(t = strchr(t, ':')) || !(t = strchr(t, '\"'))) {
        set_msg("No release found on %s", g_cfg.update_repo);
        st = UPD_ERROR;
        return NULL;
    }
    t++;
    int i = 0;
    while (t[i] && t[i] != '\"' && i < (int)sizeof(latest_tag) - 1) {
        latest_tag[i] = t[i];
        i++;
    }
    latest_tag[i] = 0;

    /* the tag is used to build a download URL later; reject anything
     * that isn't a plain version so it can't smuggle shell text */
    if (!valid_tag(latest_tag)) {
        set_msg("Release tag '%s' is not a valid version", latest_tag);
        st = UPD_ERROR;
        return NULL;
    }

    find_asset_url(body);

    long remote = ver_num(latest_tag), local = ver_num(FW_VERSION);
    if (remote < 0) {
        set_msg("Release tag '%s' is not a version", latest_tag);
        st = UPD_ERROR;
    } else if (remote > local) {
        set_msg("Version %s is available (installed: %s)",
                latest_tag, FW_VERSION);
        st = UPD_NEWER;
        event_log("SYSTEM", "Software update available: %s", latest_tag);
    } else {
        set_msg("Up to date - version %s is the newest", FW_VERSION);
        st = UPD_UPTODATE;
    }
    return NULL;
}

static void *apply_thread(void *a)
{
    (void)a;
    char auth[120], cmd[500], self[512];

    /* re-validate every externally-sourced field that feeds system():
     * repo/tag were checked in check_thread, asset_url comes from the
     * release JSON and is checked here before it hits the shell */
    if (!valid_repo(g_cfg.update_repo) || !valid_tag(latest_tag) ||
        (asset_url[0] && !url_safe(asset_url))) {
        set_msg("Update aborted - unsafe repo/tag/asset value");
        st = UPD_ERROR;
        return NULL;
    }

    auth_arg(auth, sizeof(auth));
    if (asset_url[0])
        /* API asset endpoint: valid for public and private repos */
        snprintf(cmd, sizeof(cmd),
                 "curl -sL -m 300 %s-H \"Accept: application/"
                 "octet-stream\" -o recorder_ui.update \"%s\"",
                 auth, asset_url);
    else
        snprintf(cmd, sizeof(cmd),
                 "curl -sL -m 300 %s-o recorder_ui.update "
                 "\"https://github.com/%s/releases/download/%s/%s\"",
                 auth, g_cfg.update_repo, latest_tag, UPD_ASSET);

    set_msg("Downloading %s ...", latest_tag);
    if (system(cmd) != 0) {
        set_msg("Download failed - try again");
        st = UPD_ERROR;
        return NULL;
    }

    /* sanity: a real binary, not a 404 page */
    FILE *f = fopen("recorder_ui.update", "rb");
    long sz = 0;
    if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    if (sz < 200000) {
        remove("recorder_ui.update");
        set_msg("Download invalid (asset %s missing on release %s?)",
                UPD_ASSET, latest_tag);
        st = UPD_ERROR;
        return NULL;
    }

    /* swap: running binary -> .bak, update -> binary */
#ifdef _WIN32
    if (!GetModuleFileNameA(NULL, self, sizeof(self))) {
        set_msg("Install failed (own path unknown)");
        st = UPD_ERROR;
        return NULL;
    }
    remove("recorder_ui.bak");
    if (!MoveFileExA(self, "recorder_ui.bak",
                     MOVEFILE_REPLACE_EXISTING) ||
        !MoveFileExA("recorder_ui.update", self, 0)) {
        set_msg("Install failed while swapping files");
        st = UPD_ERROR;
        return NULL;
    }
#else
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        set_msg("Install failed (own path unknown)");
        st = UPD_ERROR;
        return NULL;
    }
    self[sl] = 0;
    remove("recorder_ui.bak");
    if (rename(self, "recorder_ui.bak") != 0 ||
        rename("recorder_ui.update", self) != 0 ||
        chmod(self, 0755) != 0) {
        set_msg("Install failed while swapping files");
        st = UPD_ERROR;
        return NULL;
    }
#endif

    event_log("SYSTEM", "Software updated %s -> %s; restarting",
              FW_VERSION, latest_tag);
    set_msg("Installed %s - restarting", latest_tag);
    st = UPD_RESTARTING;

#ifdef _WIN32
    Sleep(1200);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(self, NULL, NULL, NULL, FALSE, 0, NULL, NULL,
                       &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    exit(0);
#else
    sleep(1);
    exit(0);   /* systemd Restart=always relaunches the new binary */
#endif
    return NULL;
}

void update_check_async(void)
{
    if (st == UPD_CHECKING || st == UPD_DOWNLOADING) return;
    if (!g_cfg.update_repo[0]) {
        set_msg("Update server not configured (recorder.ini"
                " [update] update_repo)");
        st = UPD_ERROR;
        return;
    }
    st = UPD_CHECKING;
    set_msg("Contacting the update server ...");
    pthread_t t;
    pthread_create(&t, NULL, check_thread, NULL);
}

void update_apply_async(void)
{
    if (st != UPD_NEWER) return;
    st = UPD_DOWNLOADING;
    pthread_t t;
    pthread_create(&t, NULL, apply_thread, NULL);
}
