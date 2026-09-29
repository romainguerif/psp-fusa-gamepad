/* PSP Bridge — file channel on USB interface 1 (see ../common/bridge_proto.h) */
#ifndef BRIDGE_H
#define BRIDGE_H

#include "usb.h"

/* fusaBridgeStatus() states */
#define BRIDGE_STATE_OFF     0 /* thread not running */
#define BRIDGE_STATE_WAITING 1 /* no USB connection yet */
#define BRIDGE_STATE_READY   2 /* configured by the host, waiting for requests */
#define BRIDGE_STATE_ERROR   3 /* could not start (see ms0:/pspbridge.log) */

/* Starts the channel thread once the USB driver is registered and started.
   `out` / `in` are the driver endpoints of the two bulk endpoints. */
int bridge_start(struct UsbEndpoint *out, struct UsbEndpoint *in);
/* Stops it; must be called before the USB driver is stopped */
void bridge_stop(void);

/* Vendor control request BRIDGE_CTRL_RESET (called from the USB bus
   driver's request callback): abandon the current exchange */
void bridge_reset(void);

/* State and number of requests served, for the loader's screen */
int bridge_status(int *requests);

/* One line in ms0:/pspbridge.log (truncated at the first line of a run) */
void bridge_log(const char *msg, unsigned int value);
/* Writes the pending log lines to ms0:/pspbridge.log (loader's thread) */
void bridge_flush_log(void);
/* Stall detector, called every ~0.5 s from the loader's thread */
void bridge_watchdog(void);
/* Same as bridge_log, from the USB bus callbacks (no file access there): queued, written
   by the bridge thread. `msg` must be a string constant. */
void bridge_note(const char *msg, unsigned int value);

#endif
