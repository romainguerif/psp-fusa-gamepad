/* PSP Bridge — file channel protocol, see bridge_proto.h */
#include "bridge_proto.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
	p[0] = v & 0xFF; p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v) {
	p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
static uint16_t get16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

void bridge_put_header(uint8_t *dst, const BridgeHeader *h) {
	put32(dst, h->magic);
	put16(dst + 4, h->cmd);
	put16(dst + 6, (uint16_t)h->status);
	put32(dst + 8, h->seq);
	put32(dst + 12, h->len);
}

void bridge_get_header(const uint8_t *src, BridgeHeader *h) {
	h->magic = get32(src);
	h->cmd = get16(src + 4);
	h->status = (int16_t)get16(src + 6);
	h->seq = get32(src + 8);
	h->len = get32(src + 12);
}

void bridge_put_hello(uint8_t *dst, const BridgeHello *h) {
	put32(dst, h->version);
	put32(dst + 4, h->maxPayload);
	put32(dst + 8, h->requests);
	put32(dst + 12, h->reserved);
	memcpy(dst + 16, h->name, sizeof(h->name));
}

void bridge_get_hello(const uint8_t *src, BridgeHello *h) {
	h->version = get32(src);
	h->maxPayload = get32(src + 4);
	h->requests = get32(src + 8);
	h->reserved = get32(src + 12);
	memcpy(h->name, src + 16, sizeof(h->name));
	h->name[sizeof(h->name) - 1] = 0;
}

int bridge_check_request(const BridgeHeader *req) {
	if (req->magic != BRIDGE_REQ_MAGIC) return BRIDGE_ERR_MAGIC;
	if (req->len > BRIDGE_MAX_PAYLOAD) return BRIDGE_ERR_TOO_BIG;
	return 0;
}

void bridge_error_response(const BridgeHeader *req, int status,
                           BridgeHeader *resp) {
	resp->magic = BRIDGE_RESP_MAGIC;
	resp->cmd = req->cmd;
	resp->status = (int16_t)status;
	resp->seq = req->seq;
	resp->len = 0;
}

void bridge_handle(const BridgeHeader *req, uint8_t *payload,
                   uint32_t requests, BridgeHeader *resp) {
	bridge_error_response(req, BRIDGE_OK, resp);
	switch (req->cmd) {
	case BRIDGE_CMD_HELLO: {
		BridgeHello h;
		memset(&h, 0, sizeof(h));
		h.version = BRIDGE_PROTO_VERSION;
		h.maxPayload = BRIDGE_MAX_PAYLOAD;
		h.requests = requests;
		memcpy(h.name, "PSP Bridge", 10);
		bridge_put_hello(payload, &h);
		resp->len = BRIDGE_HELLO_SIZE;
		break;
	}
	case BRIDGE_CMD_ECHO:
		/* the payload is already in place */
		resp->len = req->len;
		break;
	default:
		resp->status = BRIDGE_ERR_UNKNOWN;
		break;
	}
}
