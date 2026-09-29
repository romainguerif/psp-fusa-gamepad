/* PSP Bridge — libusb transport, see bridge_usb.h */
#include "bridge_usb.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSPBRIDGE_DEBUG=1 in the environment: one line per transfer */
static int debug_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("PSPBRIDGE_DEBUG") != NULL;
    return v;
}

struct BridgeUsb {
    libusb_context *ctx;
    libusb_device_handle *h;
    int iface;
    unsigned char epOut, epIn;
    int timeoutMs;
};

static const char *type_name(int attr) {
    switch (attr & LIBUSB_TRANSFER_TYPE_MASK) {
    case LIBUSB_TRANSFER_TYPE_CONTROL: return "control";
    case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: return "iso";
    case LIBUSB_TRANSFER_TYPE_BULK: return "bulk";
    default: return "interrupt";
    }
}

static const char *speed_name(int s) {
    switch (s) {
    case LIBUSB_SPEED_LOW: return "low (1.5 Mb/s)";
    case LIBUSB_SPEED_FULL: return "full (12 Mb/s)";
    case LIBUSB_SPEED_HIGH: return "high (480 Mb/s)";
    default: return "unknown";
    }
}

static libusb_device *find_psp(libusb_context *ctx, libusb_device ***listOut) {
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device *found = NULL;
    for (ssize_t i = 0; i < n && !found; i++) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) == 0 &&
            d.idVendor == PSP_VID && d.idProduct == PSP_BRIDGE_PID)
            found = list[i];
    }
    *listOut = list;
    return found;
}

int bridge_usb_describe(void) {
    libusb_context *ctx;
    libusb_device **list;
    if (libusb_init(&ctx) != 0) {
        printf("libusb_init failed\n");
        return -1;
    }
    libusb_device *dev = find_psp(ctx, &list);
    if (!dev) {
        printf("PSP not found (VID %04X PID %04X): is PSP Bridge running and the cable plugged?\n",
               PSP_VID, PSP_BRIDGE_PID);
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return -1;
    }
    struct libusb_device_descriptor d;
    libusb_get_device_descriptor(dev, &d);
    printf("PSP found: VID %04X PID %04X bcdUSB %04X bcdDevice %04X class %02X, bus %d addr %d, speed %s\n",
           d.idVendor, d.idProduct, d.bcdUSB, d.bcdDevice, d.bDeviceClass,
           libusb_get_bus_number(dev), libusb_get_device_address(dev),
           speed_name(libusb_get_device_speed(dev)));
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0) {
        printf("  no configuration descriptor\n");
    } else {
        printf("  configuration %d: wTotalLength %d, %d interface(s)\n",
               cfg->bConfigurationValue, cfg->wTotalLength, cfg->bNumInterfaces);
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            const struct libusb_interface *it = &cfg->interface[i];
            for (int a = 0; a < it->num_altsetting; a++) {
                const struct libusb_interface_descriptor *id = &it->altsetting[a];
                printf("  interface %d alt %d: class %02X/%02X/%02X, %d endpoint(s)%s\n",
                       id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass,
                       id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints,
                       id->bInterfaceClass == 3 ? "  <- gamepad (HID)"
                       : id->bInterfaceClass == 0xFF ? "  <- file channel" : "");
                for (int e = 0; e < id->bNumEndpoints; e++) {
                    const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
                    printf("    endpoint %02X %s %s, max packet %d, interval %d\n",
                           ed->bEndpointAddress,
                           (ed->bEndpointAddress & 0x80) ? "IN " : "OUT",
                           type_name(ed->bmAttributes), ed->wMaxPacketSize, ed->bInterval);
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}

BridgeUsb *bridge_usb_open(int timeoutMs, char *err, size_t errSize) {
    BridgeUsb *u = calloc(1, sizeof(*u));
    libusb_device **list = NULL;
    struct libusb_config_descriptor *cfg = NULL;
    int r;

    u->iface = -1;
    u->timeoutMs = timeoutMs;
    if (libusb_init(&u->ctx) != 0) {
        snprintf(err, errSize, "libusb_init failed");
        free(u);
        return NULL;
    }
    libusb_device *dev = find_psp(u->ctx, &list);
    if (!dev) {
        snprintf(err, errSize, "PSP not found (VID %04X PID %04X)", PSP_VID, PSP_BRIDGE_PID);
        goto fail;
    }
    /* The file channel: the vendor interface with one bulk IN and one bulk
       OUT, whatever numbers the PSP's bus driver gave them */
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 &&
        libusb_get_config_descriptor(dev, 0, &cfg) != 0) {
        snprintf(err, errSize, "cannot read the configuration descriptor");
        goto fail;
    }
    for (int i = 0; i < cfg->bNumInterfaces && u->iface < 0; i++) {
        const struct libusb_interface_descriptor *id = &cfg->interface[i].altsetting[0];
        if (id->bInterfaceClass != 0xFF) continue;
        for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ed = &id->endpoint[e];
            if ((ed->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ed->bEndpointAddress & 0x80) u->epIn = ed->bEndpointAddress;
            else u->epOut = ed->bEndpointAddress;
        }
        if (u->epIn && u->epOut) u->iface = id->bInterfaceNumber;
    }
    libusb_free_config_descriptor(cfg);
    if (u->iface < 0) {
        snprintf(err, errSize,
                 "the PSP has no file channel interface: old FuSa GamePad, or the "
                 "composite descriptor was refused (run: pspbridge info)");
        goto fail;
    }
    r = libusb_open(dev, &u->h);
    if (r != 0) {
        snprintf(err, errSize, "libusb_open: %s", libusb_error_name(r));
        goto fail;
    }
    /* macOS keeps interface 0 (HID) for the gamepad; interface 1 has no
       driver, so it can be claimed without detaching anything */
    r = libusb_claim_interface(u->h, u->iface);
    if (r != 0) {
        snprintf(err, errSize, "claim interface %d: %s", u->iface, libusb_error_name(r));
        goto fail;
    }
    libusb_free_device_list(list, 1);

    /* PSPBRIDGE_ALT=1: select the interface's setting explicitly
       (SET_INTERFACE), for bus drivers that only enable its endpoints then */
    if (getenv("PSPBRIDGE_ALT")) {
        r = libusb_set_interface_alt_setting(u->h, u->iface, 0);
        if (debug_on()) fprintf(stderr, "  SET_INTERFACE %d: %s\n", u->iface, r < 0 ? libusb_error_name(r) : "ok");
    }

    return u;

fail:
    if (list) libusb_free_device_list(list, 1);
    bridge_usb_close(u);
    return NULL;
}

void bridge_usb_resync(BridgeUsb *u) {
    static unsigned char junk[BRIDGE_MAX_PAYLOAD];
    int done, r;
    r = libusb_control_transfer(u->h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                LIBUSB_RECIPIENT_INTERFACE, BRIDGE_CTRL_RESET, 0,
                                (uint16_t)u->iface, NULL, 0, 500);
    if (debug_on()) fprintf(stderr, "  CTRL reset: %s\n", r < 0 ? libusb_error_name(r) : "ok");
    while ((r = libusb_bulk_transfer(u->h, u->epIn, junk, sizeof(junk), &done, 200)) == 0 && done > 0)
        if (debug_on()) fprintf(stderr, "  drained %d bytes\n", done);
    libusb_clear_halt(u->h, u->epOut);
    libusb_clear_halt(u->h, u->epIn);
}

BridgeUsb *bridge_usb_connect(int timeoutMs, BridgeClient *c, BridgeHello *hello,
                              char *err, size_t errSize) {
    BridgeUsb *u = bridge_usb_open(timeoutMs, err, errSize);
    if (!u) return NULL;
    bridge_client_init(c, bridge_usb_transport(u));
    int st = bridge_hello(c, hello);
    if (st != BRIDGE_OK) {
        if (debug_on()) fprintf(stderr, "  first HELLO: %s, resync\n", bridge_strerror(st));
        bridge_usb_resync(u);
        st = bridge_hello(c, hello);
    }
    if (st != BRIDGE_OK) {
        snprintf(err, errSize, "HELLO: %s", bridge_strerror(st));
        bridge_usb_close(u);
        return NULL;
    }
    return u;
}

void bridge_usb_close(BridgeUsb *u) {
    if (!u) return;
    if (u->h) {
        if (u->iface >= 0) libusb_release_interface(u->h, u->iface);
        libusb_close(u->h);
    }
    if (u->ctx) libusb_exit(u->ctx);
    free(u);
}

static int usb_write(void *ctx, const uint8_t *data, int len) {
    BridgeUsb *u = ctx;
    int done = 0;
    int r = libusb_bulk_transfer(u->h, u->epOut, (unsigned char *)data, len, &done, u->timeoutMs);
    if (debug_on()) fprintf(stderr, "  OUT %02X %d bytes: %s, %d sent\n", u->epOut, len, libusb_error_name(r), done);
    return (r == 0 && done == len) ? len : -1;
}

static int usb_read(void *ctx, uint8_t *data, int len) {
    BridgeUsb *u = ctx;
    int done = 0;
    int r = libusb_bulk_transfer(u->h, u->epIn, data, len, &done, u->timeoutMs);
    if (debug_on()) fprintf(stderr, "  IN  %02X %d bytes: %s, %d received\n", u->epIn, len, libusb_error_name(r), done);
    return (r == 0 && done == len) ? len : -1;
}

BridgeTransport bridge_usb_transport(BridgeUsb *u) {
    BridgeTransport t = { usb_write, usb_read, u };
    return t;
}
