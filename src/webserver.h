/* webserver.h - built-in read-only web server.
 *
 *   /                live dashboard (embedded single-page app)
 *   /api/live        JSON snapshot of all channels + alarms + link
 *   /api/days        JSON list of available log days
 *   /logs/<name>.csv download of a PV / alarms- / events- day file
 *
 * Read-only by design (v1): remote viewing and data download only -
 * no configuration surface. One worker thread, every socket op has a
 * timeout, so a stalled client can never block the recorder. */
#ifndef WEBSERVER_H
#define WEBSERVER_H

void webserver_init(void);    /* starts the server thread */
int  webserver_clients(void); /* requests served (for diagnostics)  */

#endif
