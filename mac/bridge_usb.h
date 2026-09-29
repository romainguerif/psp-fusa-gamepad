/* PSP Bridge — libusb transport to the PSP's file channel (interface 1) */
#ifndef BRIDGE_USB_H
#define BRIDGE_USB_H

#include "bridge_client.h"

#define PSP_VID        0x054C
#define PSP_BRIDGE_PID 0x01D2 /* FuSa GamePad's product id, kept */

typedef struct BridgeUsb BridgeUsb;

/* Prints the device and configuration descriptors as macOS sees them.
   Returns 0 if the PSP was found. */
int bridge_usb_describe(void);

/* Opens the PSP and claims the file channel interface. NULL + message in
   `err` on failure. */
BridgeUsb *bridge_usb_open(int timeoutMs, char *err, size_t errSize);
void bridge_usb_close(BridgeUsb *u);
BridgeTransport bridge_usb_transport(BridgeUsb *u);

#endif
