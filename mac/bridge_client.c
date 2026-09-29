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

int bridge_stat(BridgeClient *c, const char *path, BridgeStat *st) {
    uint8_t buf[BRIDGE_STAT_SIZE];
    uint32_t len = 0;
    size_t n = strlen(path);
    if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_CLIENT_ERR_ARG;
    int s = bridge_call(c, BRIDGE_CMD_STAT, (const uint8_t *)path, (uint32_t)n, buf, sizeof(buf), &len);
    if (s != BRIDGE_OK) return s;
    if (len != BRIDGE_STAT_SIZE) return BRIDGE_CLIENT_ERR_REPLY;
    st->type = bridge_get_u32(buf);
    st->size = bridge_get_u64(buf + 8);
    return BRIDGE_OK;
}

int bridge_list(BridgeClient *c, const char *path, BridgeListFn fn, void *user) {
    size_t n = strlen(path);
    if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_CLIENT_ERR_ARG;
    uint8_t req[4 + BRIDGE_PATH_MAX];
    uint8_t *out = malloc(BRIDGE_MAX_PAYLOAD);
    if (!out) return BRIDGE_CLIENT_ERR_IO;
    uint32_t start = 0;
    int st = BRIDGE_OK;
    for (;;) {
        uint32_t len = 0;
        bridge_put_u32(req, start);
        memcpy(req + 4, path, n);
        st = bridge_call(c, BRIDGE_CMD_LIST, req, (uint32_t)(4 + n), out, BRIDGE_MAX_PAYLOAD, &len);
        if (st != BRIDGE_OK) break;
        if (len < BRIDGE_LIST_HEADER) { st = BRIDGE_CLIENT_ERR_REPLY; break; }
        uint32_t count = bridge_get_u32(out), more = bridge_get_u32(out + 4);
        uint32_t pos = BRIDGE_LIST_HEADER;
        int stop = 0;
        for (uint32_t i = 0; i < count && !stop; i++) {
            if (pos + BRIDGE_ENTRY_HEADER > len) { st = BRIDGE_CLIENT_ERR_REPLY; stop = 1; break; }
            BridgeEntry e;
            uint32_t nameLen = out[pos + 1];
            if (pos + BRIDGE_ENTRY_HEADER + nameLen > len) { st = BRIDGE_CLIENT_ERR_REPLY; stop = 1; break; }
            memset(&e, 0, sizeof(e));
            e.type = out[pos];
            e.size = bridge_get_u64(out + pos + 4);
            memcpy(e.name, out + pos + BRIDGE_ENTRY_HEADER, nameLen);
            pos += BRIDGE_ENTRY_HEADER + nameLen;
            if (fn(user, &e)) stop = 1;
        }
        if (stop || !more || count == 0) break;
        start += count;
    }
    free(out);
    return st;
}

int bridge_read(BridgeClient *c, const char *path, uint64_t offset, uint32_t len,
                uint8_t *dst, uint32_t *got) {
    size_t n = strlen(path);
    *got = 0;
    if (n == 0 || n >= BRIDGE_PATH_MAX || len > BRIDGE_MAX_PAYLOAD) return BRIDGE_CLIENT_ERR_ARG;
    uint8_t req[BRIDGE_READ_ARGS + BRIDGE_PATH_MAX];
    bridge_put_u64(req, offset);
    bridge_put_u32(req + 8, len);
    memcpy(req + BRIDGE_READ_ARGS, path, n);
    return bridge_call(c, BRIDGE_CMD_READ, req, (uint32_t)(BRIDGE_READ_ARGS + n), dst, len, got);
}

int bridge_write_file(BridgeClient *c, const char *path, const uint8_t *data, uint32_t len) {
    size_t n = strlen(path);
    if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_CLIENT_ERR_ARG;
    uint32_t room = BRIDGE_MAX_PAYLOAD - BRIDGE_WRITE_ARGS - (uint32_t)n;
    uint8_t *req = malloc(BRIDGE_MAX_PAYLOAD);
    if (!req) return BRIDGE_CLIENT_ERR_IO;
    uint32_t done = 0;
    int st = BRIDGE_OK;
    do {
        uint32_t piece = len - done < room ? len - done : room;
        uint8_t reply[4];
        uint32_t got = 0;
        bridge_put_u64(req, done);
        bridge_put_u32(req + 8, done == 0 ? BRIDGE_WRITE_TRUNCATE : 0);
        req[12] = (uint8_t)n;
        req[13] = (uint8_t)(n >> 8);
        req[14] = req[15] = 0;
        memcpy(req + BRIDGE_WRITE_ARGS, path, n);
        memcpy(req + BRIDGE_WRITE_ARGS + n, data + done, piece);
        st = bridge_call(c, BRIDGE_CMD_WRITE, req, (uint32_t)(BRIDGE_WRITE_ARGS + n + piece), reply,
                         sizeof(reply), &got);
        if (st == BRIDGE_OK && (got != 4 || bridge_get_u32(reply) != piece)) st = BRIDGE_CLIENT_ERR_REPLY;
        done += piece;
    } while (st == BRIDGE_OK && done < len);
    free(req);
    return st;
}

static int path_call(BridgeClient *c, uint16_t cmd, const char *path) {
    size_t n = strlen(path);
    if (n == 0 || n >= BRIDGE_PATH_MAX) return BRIDGE_CLIENT_ERR_ARG;
    return bridge_call(c, cmd, (const uint8_t *)path, (uint32_t)n, NULL, 0, NULL);
}

int bridge_mkdir(BridgeClient *c, const char *path) { return path_call(c, BRIDGE_CMD_MKDIR, path); }
int bridge_remove(BridgeClient *c, const char *path) { return path_call(c, BRIDGE_CMD_REMOVE, path); }

int bridge_rename(BridgeClient *c, const char *from, const char *to) {
    size_t a = strlen(from), b = strlen(to);
    if (!a || !b || a >= BRIDGE_PATH_MAX || b >= BRIDGE_PATH_MAX) return BRIDGE_CLIENT_ERR_ARG;
    uint8_t req[4 + 2 * BRIDGE_PATH_MAX];
    req[0] = (uint8_t)a;
    req[1] = (uint8_t)(a >> 8);
    req[2] = req[3] = 0;
    memcpy(req + 4, from, a);
    memcpy(req + 4 + a, to, b);
    return bridge_call(c, BRIDGE_CMD_RENAME, req, (uint32_t)(4 + a + b), NULL, 0, NULL);
}

int bridge_read_file(BridgeClient *c, const char *path, uint8_t **data, uint32_t *len) {
    BridgeStat st;
    *data = NULL;
    *len = 0;
    int r = bridge_stat(c, path, &st);
    if (r != BRIDGE_OK) return r;
    if (st.type != BRIDGE_TYPE_FILE || st.size > (64u << 20)) return BRIDGE_CLIENT_ERR_ARG;
    uint8_t *buf = malloc(st.size ? (size_t)st.size : 1);
    if (!buf) return BRIDGE_CLIENT_ERR_IO;
    uint32_t done = 0;
    while (done < st.size) {
        uint32_t want = (uint32_t)(st.size - done < BRIDGE_MAX_PAYLOAD ? st.size - done : BRIDGE_MAX_PAYLOAD);
        uint32_t got = 0;
        r = bridge_read(c, path, done, want, buf + done, &got);
        if (r != BRIDGE_OK || got == 0) break;
        done += got;
    }
    if (r == BRIDGE_OK && done != st.size) r = BRIDGE_CLIENT_ERR_REPLY;
    if (r != BRIDGE_OK) {
        free(buf);
        return r;
    }
    *data = buf;
    *len = done;
    return BRIDGE_OK;
}

const char *bridge_strerror(int status) {
    switch (status) {
    case BRIDGE_OK: return "ok";
    case BRIDGE_ERR_MAGIC: return "PSP: request not recognised";
    case BRIDGE_ERR_TOO_BIG: return "PSP: payload too big";
    case BRIDGE_ERR_UNKNOWN: return "PSP: unknown command";
    case BRIDGE_ERR_PATH: return "PSP: path refused";
    case BRIDGE_ERR_NOENT: return "PSP: no such file";
    case BRIDGE_ERR_IO: return "PSP: Memory Stick error";
    case BRIDGE_ERR_ARGS: return "PSP: malformed request";
    case BRIDGE_ERR_DENIED: return "PSP: writing there is not allowed";
    case BRIDGE_ERR_EXIST: return "PSP: target already exists";
    case BRIDGE_CLIENT_ERR_IO: return "USB transfer failed or timed out";
    case BRIDGE_CLIENT_ERR_REPLY: return "unexpected reply";
    case BRIDGE_CLIENT_ERR_ARG: return "bad argument";
    default: return "unknown error";
    }
}
