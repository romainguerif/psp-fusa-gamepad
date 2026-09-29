/*
 * PSP Bridge — Mac side of the file channel (see ../src/common/bridge_proto.h).
 * The client only knows a byte transport: libusb on the real PSP
 * (bridge_usb.c), or an in-process fake PSP for the tests (fake_psp.c).
 */
#ifndef BRIDGE_CLIENT_H
#define BRIDGE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "../src/common/bridge_proto.h"

typedef struct {
    /* Each call is one USB transfer of exactly len bytes. Return len on
       success, < 0 on error or timeout. */
    int (*write)(void *ctx, const uint8_t *data, int len);
    int (*read)(void *ctx, uint8_t *data, int len);
    void *ctx;
} BridgeTransport;

/* Errors of the client itself (the PSP's own are BRIDGE_ERR_*) */
#define BRIDGE_CLIENT_ERR_IO    -100 /* transfer failed / timeout */
#define BRIDGE_CLIENT_ERR_REPLY -101 /* malformed or unexpected reply */
#define BRIDGE_CLIENT_ERR_ARG   -102

typedef struct {
    BridgeTransport t;
    uint32_t seq;
} BridgeClient;

void bridge_client_init(BridgeClient *c, BridgeTransport t);

/*
 * One request/response. `out` receives up to `outCap` payload bytes, the
 * real size in *outLen. Returns the PSP's status (BRIDGE_OK or
 * BRIDGE_ERR_*), or BRIDGE_CLIENT_ERR_*.
 */
int bridge_call(BridgeClient *c, uint16_t cmd, const uint8_t *in, uint32_t inLen,
                uint8_t *out, uint32_t outCap, uint32_t *outLen);

int bridge_hello(BridgeClient *c, BridgeHello *hello);

/* Sends `len` bytes of a pattern derived from `seed`, checks they come
   back unchanged. Returns BRIDGE_OK, a PSP error, a client error, or
   BRIDGE_CLIENT_ERR_REPLY on a data mismatch. */
int bridge_echo_check(BridgeClient *c, uint32_t len, uint32_t seed);

const char *bridge_strerror(int status);

#endif
