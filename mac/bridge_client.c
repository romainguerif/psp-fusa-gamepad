/* PSP Bridge — Mac side of the file channel, see bridge_client.h */
#include "bridge_client.h"

#include <stdlib.h>
#include <string.h>

void bridge_client_init(BridgeClient *c, BridgeTransport t) {
    c->t = t;
    c->seq = 1;
}

int bridge_call(BridgeClient *c, uint16_t cmd, const uint8_t *in, uint32_t inLen,
                uint8_t *out, uint32_t outCap, uint32_t *outLen) {
    uint8_t hdr[BRIDGE_HEADER_SIZE];
    BridgeHeader req, resp;

    if (outLen) *outLen = 0;
    if (inLen > BRIDGE_MAX_PAYLOAD || (inLen && !in)) return BRIDGE_CLIENT_ERR_ARG;

    req.magic = BRIDGE_REQ_MAGIC;
    req.cmd = cmd;
    req.status = 0;
    req.seq = c->seq++;
    req.len = inLen;
    bridge_put_header(hdr, &req);
    if (c->t.write(c->t.ctx, hdr, BRIDGE_HEADER_SIZE) != BRIDGE_HEADER_SIZE)
        return BRIDGE_CLIENT_ERR_IO;
    if (inLen && c->t.write(c->t.ctx, in, (int)inLen) != (int)inLen)
        return BRIDGE_CLIENT_ERR_IO;

    if (c->t.read(c->t.ctx, hdr, BRIDGE_HEADER_SIZE) != BRIDGE_HEADER_SIZE)
        return BRIDGE_CLIENT_ERR_IO;
    bridge_get_header(hdr, &resp);
    if (resp.magic != BRIDGE_RESP_MAGIC || resp.len > BRIDGE_MAX_PAYLOAD)
        return BRIDGE_CLIENT_ERR_REPLY;
    /* A bad-magic answer can't carry our sequence number: the PSP never
       understood the request */
    if (resp.status != BRIDGE_ERR_MAGIC && (resp.seq != req.seq || resp.cmd != cmd))
        return BRIDGE_CLIENT_ERR_REPLY;

    if (resp.len) {
        /* Always drain the whole payload so the next call starts clean */
        uint8_t *dst = out;
        uint8_t *tmp = NULL;
        if (resp.len > outCap || !out) {
            tmp = malloc(resp.len);
            if (!tmp) return BRIDGE_CLIENT_ERR_IO;
            dst = tmp;
        }
        int n = c->t.read(c->t.ctx, dst, (int)resp.len);
        if (tmp) {
            if (out && outCap) memcpy(out, tmp, outCap);
            free(tmp);
        }
        if (n != (int)resp.len) return BRIDGE_CLIENT_ERR_IO;
        if (resp.len > outCap) return BRIDGE_CLIENT_ERR_REPLY;
    }
    if (outLen) *outLen = resp.len;
    return resp.status;
}

int bridge_hello(BridgeClient *c, BridgeHello *hello) {
    uint8_t buf[BRIDGE_HELLO_SIZE];
    uint32_t len = 0;
    int st = bridge_call(c, BRIDGE_CMD_HELLO, NULL, 0, buf, sizeof(buf), &len);
    if (st != BRIDGE_OK) return st;
    if (len != BRIDGE_HELLO_SIZE) return BRIDGE_CLIENT_ERR_REPLY;
    bridge_get_hello(buf, hello);
    return BRIDGE_OK;
}

static void fill_pattern(uint8_t *p, uint32_t len, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < len; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

int bridge_echo_check(BridgeClient *c, uint32_t len, uint32_t seed) {
    uint8_t *in = malloc(len ? len : 1);
    uint8_t *out = malloc(len ? len : 1);
    uint32_t got = 0;
    int st;
    if (!in || !out) {
        free(in); free(out);
        return BRIDGE_CLIENT_ERR_IO;
    }
    fill_pattern(in, len, seed);
    st = bridge_call(c, BRIDGE_CMD_ECHO, in, len, out, len, &got);
    if (st == BRIDGE_OK && (got != len || memcmp(in, out, len) != 0))
        st = BRIDGE_CLIENT_ERR_REPLY;
    free(in);
    free(out);
    return st;
}

const char *bridge_strerror(int status) {
    switch (status) {
    case BRIDGE_OK: return "ok";
    case BRIDGE_ERR_MAGIC: return "PSP: request not recognised";
    case BRIDGE_ERR_TOO_BIG: return "PSP: payload too big";
    case BRIDGE_ERR_UNKNOWN: return "PSP: unknown command";
    case BRIDGE_CLIENT_ERR_IO: return "USB transfer failed or timed out";
    case BRIDGE_CLIENT_ERR_REPLY: return "unexpected reply";
    case BRIDGE_CLIENT_ERR_ARG: return "bad argument";
    default: return "unknown error";
    }
}
