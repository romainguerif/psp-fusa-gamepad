/* PSP Bridge — libusb transport to the PSP's file channel (interface 1) */
#ifndef BRIDGE_USB_H
#define BRIDGE_USB_H

#include "bridge_client.h"

#define PSP_VID        0x054C
#define PSP_BRIDGE_PID 0x01D2 /* FuSa GamePad's product id (first versions) */
/* PSP Bridge's own identity since 29/09: not Sony's, so that macOS 26's
   GameController framework leaves the gamepad to SDL/PPSSPP */
#define BRIDGE_VID     0x1209
#define BRIDGE_PID     0x5053
/* The PSP's bus keeps Sony's vendor id whatever the descriptor says: in
   practice PSP Bridge is 054C:5053 */
#define IS_PSP_BRIDGE(v, p) \
    (((v) == PSP_VID || (v) == BRIDGE_VID) && ((p) == BRIDGE_PID || (p) == PSP_BRIDGE_PID))

typedef struct BridgeUsb BridgeUsb;

/* Prints the device and configuration descriptors as macOS sees them.
   Returns 0 if the PSP was found. */
int bridge_usb_describe(void);

/* Opens the PSP and claims the file channel interface. NULL + message in
   `err` on failure. */
BridgeUsb *bridge_usb_open(int timeoutMs, char *err, size_t errSize);
void bridge_usb_close(BridgeUsb *u);
/* Brings the channel back in step after a previous client died mid-exchange:
   asks the PSP to drop the exchange (vendor request), then throws away what
   it still had queued. Use it when a first HELLO fails. */
void bridge_usb_resync(BridgeUsb *u);
/* Opens + HELLO, with one resync if needed. NULL + message on failure. */
BridgeUsb *bridge_usb_connect(int timeoutMs, BridgeClient *c, BridgeHello *hello,
                              char *err, size_t errSize);
BridgeTransport bridge_usb_transport(BridgeUsb *u);

#endif
