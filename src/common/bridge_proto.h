/*
 * PSP Bridge — file channel protocol (USB interface 1), shared by the PSP
 * driver and the Mac tools. Pure C, no PSP or libusb dependency: the same
 * file is compiled into usbgamepad.prx and into the Mac tests.
 *
 * Transport: two bulk endpoints of interface 1 (vendor class).
 *   Mac -> PSP : bulk OUT 0x02    PSP -> Mac : bulk IN 0x83
 *
 * Every message is a 16-byte header, sent as its own USB transfer, then
 * `len` bytes of payload as a second transfer (only if len > 0). Both sides
 * always read exact sizes, so packet boundaries never need a zero-length
 * packet (same scheme as psplinkusb's usbhostfs).
 *
 * Header, all fields little endian:
 *   u32 magic   BRIDGE_REQ_MAGIC (Mac -> PSP) or BRIDGE_RESP_MAGIC
 *   u16 cmd     BRIDGE_CMD_*; a response repeats the request's command
 *   s16 status  0 in requests; BRIDGE_OK or BRIDGE_ERR_* in responses
 *   u32 seq     chosen by the Mac, echoed in the response
 *   u32 len     payload bytes that follow, <= BRIDGE_MAX_PAYLOAD
 */
#ifndef BRIDGE_PROTO_H
#define BRIDGE_PROTO_H

#include <stdint.h>

#define BRIDGE_PROTO_VERSION 3 /* 2: STAT, LIST, READ; 3: save writes */

#define BRIDGE_REQ_MAGIC  0x51524250u /* "PBRQ" */
#define BRIDGE_RESP_MAGIC 0x53524250u /* "PBRS" */

#define BRIDGE_HEADER_SIZE 16
#define BRIDGE_MAX_PAYLOAD (64 * 1024)

/* Vendor control request (EP0, host to device, interface 1, no data): the
   PSP abandons any half-done exchange and waits for a new header. The Mac
   sends it when it opens the channel, then drains what the PSP still had
   to send. */
#define BRIDGE_CTRL_RESET 0x01

/* Commands */
#define BRIDGE_CMD_HELLO 1 /* -> BridgeHello payload */
#define BRIDGE_CMD_ECHO  2 /* payload sent back unchanged */
#define BRIDGE_CMD_STAT  3 /* path -> BridgeStat (BRIDGE_STAT_SIZE bytes) */
#define BRIDGE_CMD_LIST  4 /* u32 start, path -> BridgeList + entries */
#define BRIDGE_CMD_READ  5 /* u64 offset, u32 len, path -> data (short at EOF) */
/* Writes: only under ms0:/PSP/SAVEDATA/ (bridge_path_writable) */
#define BRIDGE_CMD_WRITE  6 /* u64 offset, u32 flags, u16 pathLen, u16 0, path, data */
#define BRIDGE_CMD_MKDIR  7 /* path (already there = ok) */
#define BRIDGE_CMD_RENAME 8 /* u16 fromLen, u16 0, from, to (to must not exist) */
#define BRIDGE_CMD_REMOVE 9 /* path of a file */
#define BRIDGE_WRITE_TRUNCATE 1 /* flags: create / empty the file first */
#define BRIDGE_WRITE_ARGS 16
#define BRIDGE_SAVEDATA "ms0:/PSP/SAVEDATA/"

/* Status */
#define BRIDGE_OK            0
#define BRIDGE_ERR_MAGIC    -1 /* header not recognised: the Mac must resync */
#define BRIDGE_ERR_TOO_BIG  -2
#define BRIDGE_ERR_UNKNOWN  -3 /* unknown command */
#define BRIDGE_ERR_PATH     -4 /* path refused: not under ms0:/, "..", too long */
#define BRIDGE_ERR_NOENT    -5 /* no such file or directory */
#define BRIDGE_ERR_IO       -6 /* Memory Stick error */
#define BRIDGE_ERR_ARGS     -7 /* malformed request payload */
#define BRIDGE_ERR_DENIED   -8 /* write outside ms0:/PSP/SAVEDATA/ */
#define BRIDGE_ERR_EXIST    -9 /* rename target already there */

typedef struct {
	uint32_t magic;
	uint16_t cmd;
	int16_t status;
	uint32_t seq;
	uint32_t len;
} BridgeHeader;

/* HELLO response payload (BRIDGE_HELLO_SIZE bytes, little endian) */
#define BRIDGE_HELLO_SIZE 32
typedef struct {
	uint32_t version;     /* BRIDGE_PROTO_VERSION */
	uint32_t maxPayload;  /* BRIDGE_MAX_PAYLOAD */
	uint32_t requests;    /* requests served since the driver started */
	uint32_t reserved;
	char name[16];        /* "PSP Bridge", NUL padded */
} BridgeHello;

/* Paths are sent without NUL, at the end of the payload: "ms0:/ISO/x.iso" */
#define BRIDGE_PATH_MAX 256

#define BRIDGE_TYPE_NONE 0
#define BRIDGE_TYPE_FILE 1
#define BRIDGE_TYPE_DIR  2

/* STAT response payload: u32 type, u32 reserved, u64 size */
#define BRIDGE_STAT_SIZE 16
typedef struct {
	uint32_t type;
	uint64_t size;
} BridgeStat;

/* LIST response payload: u32 count, u32 more (1 = ask again with start +
   count), then `count` entries: u8 type, u8 nameLen, u16 0, u64 size, name */
#define BRIDGE_LIST_HEADER 8
#define BRIDGE_ENTRY_HEADER 12
typedef struct {
	uint32_t type;
	uint64_t size;
	char name[256];
} BridgeEntry;

/* READ request payload: u64 offset, u32 len (<= BRIDGE_MAX_PAYLOAD), path */
#define BRIDGE_READ_ARGS 12

/*
 * Memory Stick access used by the commands: sceIo on the PSP
 * (src/prx/psp_fs.c), a local folder in the fake PSP. Paths are already
 * checked (ms0:/..., no ".."). Return 0 / bytes, or a BRIDGE_ERR_*.
 */
typedef int (*BridgeListFn)(void *user, const BridgeEntry *e); /* 1 = stop */
typedef struct {
	int (*stat)(void *ctx, const char *path, BridgeStat *st);
	int (*list)(void *ctx, const char *path, BridgeListFn fn, void *user);
	int (*read)(void *ctx, const char *path, uint64_t offset, uint8_t *dst, uint32_t len);
	/* writes: return bytes written / 0, or a BRIDGE_ERR_* */
	int (*write)(void *ctx, const char *path, uint64_t offset, const uint8_t *src,
	             uint32_t len, int truncate);
	int (*mkdir)(void *ctx, const char *path);
	int (*rename)(void *ctx, const char *from, const char *to);
	int (*remove)(void *ctx, const char *path);
	void *ctx;
} BridgeFs;

/* 1 if `path` may be read: starts with "ms0:/", no "." or ".." component,
   no backslash or control character, shorter than BRIDGE_PATH_MAX */
int bridge_path_ok(const char *path);
/* 1 if `path` may be written: bridge_path_ok and strictly inside
   ms0:/PSP/SAVEDATA/ */
int bridge_path_writable(const char *path);

void bridge_put_header(uint8_t *dst, const BridgeHeader *h);
void bridge_get_header(const uint8_t *src, BridgeHeader *h);

void bridge_put_hello(uint8_t *dst, const BridgeHello *h);
void bridge_get_hello(const uint8_t *src, BridgeHello *h);

/*
 * PSP side. `req` is a received header. Returns 0 if its payload (req->len
 * bytes) must now be read into the payload buffer, or a BRIDGE_ERR_* code:
 * then no payload is read and bridge_error_response() gives the answer.
 */
int bridge_check_request(const BridgeHeader *req);

/*
 * PSP side, once the payload is in `payload` (capacity BRIDGE_MAX_PAYLOAD).
 * Fills `resp`; the response payload (resp->len bytes) is written in place
 * into `payload`. `requests` is the counter reported by HELLO.
 */
void bridge_handle(const BridgeHeader *req, uint8_t *payload,
                   uint32_t requests, const BridgeFs *fs, BridgeHeader *resp);

/* Payload helpers, shared with the Mac client */
void bridge_put_u32(uint8_t *p, uint32_t v);
void bridge_put_u64(uint8_t *p, uint64_t v);
uint32_t bridge_get_u32(const uint8_t *p);
uint64_t bridge_get_u64(const uint8_t *p);

void bridge_error_response(const BridgeHeader *req, int status,
                           BridgeHeader *resp);

#endif
