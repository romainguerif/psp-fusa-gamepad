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

void bridge_put_u32(uint8_t *p, uint32_t v) { put32(p, v); }
uint32_t bridge_get_u32(const uint8_t *p) { return get32(p); }
void bridge_put_u64(uint8_t *p, uint64_t v) {
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}
uint64_t bridge_get_u64(const uint8_t *p) {
	return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

int bridge_path_ok(const char *path) {
	int n = 0;
	const char *comp;
	if (strncmp(path, "ms0:/", 5) != 0) return 0;
	for (n = 0; path[n]; n++) {
		unsigned char c = (unsigned char)path[n];
		if (n >= BRIDGE_PATH_MAX - 1 || c < 0x20 || c == '\\' || c == 0x7F) return 0;
	}
	/* no "." or ".." component (empty ones, "//", are harmless) */
	comp = path + 5;
	while (*comp) {
		const char *end = comp;
		while (*end && *end != '/') end++;
		if ((end - comp == 1 && comp[0] == '.') ||
		    (end - comp == 2 && comp[0] == '.' && comp[1] == '.'))
			return 0;
		comp = *end ? end + 1 : end;
	}
	return 1;
}

int bridge_path_writable(const char *path) {
	size_t n = strlen(BRIDGE_SAVEDATA);
	return bridge_path_ok(path) && strncmp(path, BRIDGE_SAVEDATA, n) == 0 && path[n] != 0;
}

/* Copies n bytes at payload+at into `path` and checks it; 0 or an error */
static int take_path_n(const uint8_t *payload, uint32_t at, uint32_t n, char *path) {
	if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_ERR_PATH;
	memcpy(path, payload + at, n);
	path[n] = 0;
	if (strlen(path) != n) return BRIDGE_ERR_PATH;
	return bridge_path_ok(path) ? 0 : BRIDGE_ERR_PATH;
}

/* Copies the path found at payload[at..len) into `path`; 0 or an error */
static int take_path(const uint8_t *payload, uint32_t len, uint32_t at, char *path) {
	uint32_t n;
	if (at > len) return BRIDGE_ERR_ARGS;
	n = len - at;
	if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_ERR_PATH;
	memcpy(path, payload + at, n);
	path[n] = 0;
	if (strlen(path) != n) return BRIDGE_ERR_PATH; /* embedded NUL */
	return bridge_path_ok(path) ? 0 : BRIDGE_ERR_PATH;
}

typedef struct {
	uint8_t *out;     /* response payload */
	uint32_t used;    /* bytes written, header included */
	uint32_t skip;    /* entries still to skip (start) */
	uint32_t count;
	int more;
} ListState;

static int list_emit(void *user, const BridgeEntry *e) {
	ListState *ls = user;
	uint32_t nameLen = (uint32_t)strlen(e->name);
	uint8_t *p;
	if (nameLen > 255) nameLen = 255;
	if (ls->skip) {
		ls->skip--;
		return 0;
	}
	if (ls->used + BRIDGE_ENTRY_HEADER + nameLen > BRIDGE_MAX_PAYLOAD) {
		ls->more = 1;
		return 1;
	}
	p = ls->out + ls->used;
	p[0] = (uint8_t)e->type;
	p[1] = (uint8_t)nameLen;
	p[2] = p[3] = 0;
	bridge_put_u64(p + 4, e->size);
	memcpy(p + BRIDGE_ENTRY_HEADER, e->name, nameLen);
	ls->used += BRIDGE_ENTRY_HEADER + nameLen;
	ls->count++;
	return 0;
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
                   uint32_t requests, const BridgeFs *fs, BridgeHeader *resp) {
	char path[BRIDGE_PATH_MAX];
	int err;

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
	case BRIDGE_CMD_STAT: {
		BridgeStat st;
		err = take_path(payload, req->len, 0, path);
		if (!err) err = fs->stat(fs->ctx, path, &st);
		if (err) {
			resp->status = (int16_t)err;
			break;
		}
		bridge_put_u32(payload, st.type);
		bridge_put_u32(payload + 4, 0);
		bridge_put_u64(payload + 8, st.size);
		resp->len = BRIDGE_STAT_SIZE;
		break;
	}
	case BRIDGE_CMD_LIST: {
		ListState ls;
		if (req->len < 4) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		err = take_path(payload, req->len, 4, path);
		if (err) {
			resp->status = (int16_t)err;
			break;
		}
		memset(&ls, 0, sizeof(ls));
		ls.out = payload;
		ls.used = BRIDGE_LIST_HEADER;
		ls.skip = bridge_get_u32(payload);
		err = fs->list(fs->ctx, path, list_emit, &ls);
		if (err) {
			resp->status = (int16_t)err;
			break;
		}
		bridge_put_u32(payload, ls.count);
		bridge_put_u32(payload + 4, (uint32_t)ls.more);
		resp->len = ls.used;
		break;
	}
	case BRIDGE_CMD_READ: {
		uint64_t offset;
		uint32_t len;
		int n;
		if (req->len < BRIDGE_READ_ARGS) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		offset = bridge_get_u64(payload);
		len = bridge_get_u32(payload + 8);
		err = take_path(payload, req->len, BRIDGE_READ_ARGS, path);
		if (!err && len > BRIDGE_MAX_PAYLOAD) err = BRIDGE_ERR_ARGS;
		if (err) {
			resp->status = (int16_t)err;
			break;
		}
		n = fs->read(fs->ctx, path, offset, payload, len);
		if (n < 0) {
			resp->status = (int16_t)n;
			break;
		}
		resp->len = (uint32_t)n;
		break;
	}
	case BRIDGE_CMD_WRITE: {
		uint64_t offset;
		uint32_t flags, pathLen;
		int n;
		if (req->len < BRIDGE_WRITE_ARGS) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		offset = bridge_get_u64(payload);
		flags = bridge_get_u32(payload + 8);
		pathLen = get16(payload + 12);
		if (BRIDGE_WRITE_ARGS + pathLen > req->len) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		err = take_path_n(payload, BRIDGE_WRITE_ARGS, pathLen, path);
		if (!err && !bridge_path_writable(path)) err = BRIDGE_ERR_DENIED;
		if (err) {
			resp->status = (int16_t)err;
			break;
		}
		n = fs->write(fs->ctx, path, offset, payload + BRIDGE_WRITE_ARGS + pathLen,
		              req->len - BRIDGE_WRITE_ARGS - pathLen, (flags & BRIDGE_WRITE_TRUNCATE) != 0);
		if (n < 0) {
			resp->status = (int16_t)n;
			break;
		}
		bridge_put_u32(payload, (uint32_t)n);
		resp->len = 4;
		break;
	}
	case BRIDGE_CMD_MKDIR:
	case BRIDGE_CMD_REMOVE:
		err = take_path(payload, req->len, 0, path);
		if (!err && !bridge_path_writable(path)) err = BRIDGE_ERR_DENIED;
		if (!err)
			err = req->cmd == BRIDGE_CMD_MKDIR ? fs->mkdir(fs->ctx, path) : fs->remove(fs->ctx, path);
		resp->status = (int16_t)err;
		break;
	case BRIDGE_CMD_RENAME: {
		char to[BRIDGE_PATH_MAX];
		uint32_t fromLen;
		if (req->len < 4) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		fromLen = get16(payload);
		if (4 + fromLen > req->len) {
			resp->status = BRIDGE_ERR_ARGS;
			break;
		}
		err = take_path_n(payload, 4, fromLen, path);
		if (!err) err = take_path(payload, req->len, 4 + fromLen, to);
		if (!err && (!bridge_path_writable(path) || !bridge_path_writable(to))) err = BRIDGE_ERR_DENIED;
		if (!err) err = fs->rename(fs->ctx, path, to);
		resp->status = (int16_t)err;
		break;
	}
	default:
		resp->status = BRIDGE_ERR_UNKNOWN;
		break;
	}
}
