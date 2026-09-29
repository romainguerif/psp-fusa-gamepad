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

#define BRIDGE_PROTO_VERSION 1

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

/* Status */
#define BRIDGE_OK            0
#define BRIDGE_ERR_MAGIC    -1 /* header not recognised: the Mac must resync */
#define BRIDGE_ERR_TOO_BIG  -2
#define BRIDGE_ERR_UNKNOWN  -3 /* unknown command */

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
                   uint32_t requests, BridgeHeader *resp);

void bridge_error_response(const BridgeHeader *req, int status,
                           BridgeHeader *resp);

#endif
