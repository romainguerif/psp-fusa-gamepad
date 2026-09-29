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

/* File commands (protocol 2). Paths: "ms0:/ISO/game.iso" */
int bridge_stat(BridgeClient *c, const char *path, BridgeStat *st);
/* Calls fn for every entry of the folder (asks again while the PSP says
   "more"). fn returns 1 to stop. */
int bridge_list(BridgeClient *c, const char *path, BridgeListFn fn, void *user);
/* Reads up to len (<= BRIDGE_MAX_PAYLOAD) bytes at offset; *got = bytes
   read (short at end of file) */
int bridge_read(BridgeClient *c, const char *path, uint64_t offset, uint32_t len,
                uint8_t *dst, uint32_t *got);

/* Save writes (protocol 3), only under ms0:/PSP/SAVEDATA/. len may exceed
   one request: the data is sent in BRIDGE_MAX_PAYLOAD pieces. */
int bridge_write_file(BridgeClient *c, const char *path, const uint8_t *data, uint32_t len);
int bridge_mkdir(BridgeClient *c, const char *path);
int bridge_rename(BridgeClient *c, const char *from, const char *to);
int bridge_remove(BridgeClient *c, const char *path);
/* Reads a whole file (malloc'd into *data, size in *len) */
int bridge_read_file(BridgeClient *c, const char *path, uint8_t **data, uint32_t *len);

const char *bridge_strerror(int status);

#endif
