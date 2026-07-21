/* update.h - over-the-air software update from GitHub Releases.
 *
 * Server = a GitHub repository (recorder.ini [update] update_repo,
 * e.g. "jetpace/pr40-firmware"; update_token for private repos).
 * Each release is a tag "vX.Y.Z" carrying one binary asset per
 * platform: "recorder_ui" (Pi) / "recorder_ui.exe" (Windows sim).
 *
 * Flow: check (GitHub API, version compare) -> download the asset ->
 * sanity check -> swap binaries keeping the old one as .bak ->
 * restart. Both steps run in worker threads; the UI polls
 * update_state()/update_message(). */
#ifndef UPDATE_H
#define UPDATE_H

typedef enum {
    UPD_IDLE = 0,
    UPD_CHECKING,      /* talking to the update server   */
    UPD_UPTODATE,      /* no newer release               */
    UPD_NEWER,         /* newer release found            */
    UPD_DOWNLOADING,   /* fetching + installing          */
    UPD_RESTARTING,    /* installed, about to restart    */
    UPD_ERROR
} upd_state_t;

upd_state_t update_state(void);
const char *update_message(void);   /* human-readable status line */

void update_check_async(void);      /* -> UPTODATE / NEWER / ERROR */
void update_apply_async(void);      /* only valid in UPD_NEWER     */

#endif
