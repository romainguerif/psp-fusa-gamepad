/* In-process fake PSP, see fake_psp.h. Mirrors bridge_thread() of
   ../src/prx/bridge.c: header transfer, payload transfer, answer. */
#include "fake_psp.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct FakePsp {
    /* what the PSP expects next from the host */
    int wantPayload;
    BridgeHeader req;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];
    unsigned requests;
    char root[1024];
    int hasRoot;
    BridgeFs fs;
    /* what the PSP has queued for the host: whole transfers */
    uint8_t out[BRIDGE_HEADER_SIZE + BRIDGE_MAX_PAYLOAD];
    int outHdr;     /* bytes of header waiting (0 or 16) */
    int outPayload; /* bytes of payload waiting */
};

/* --- the Memory Stick: a local folder ------------------------------------- */

static int host_path(FakePsp *f, const char *path, char *out, size_t size) {
    if (!f->hasRoot) return BRIDGE_ERR_IO;
    /* path was checked by bridge_path_ok: "ms0:/..." without ".." */
    snprintf(out, size, "%s/%s", f->root, path + 5);
    return 0;
}

static int fs_stat(void *ctx, const char *path, BridgeStat *st) {
    char p[2048];
    struct stat s;
    if (host_path(ctx, path, p, sizeof(p))) return BRIDGE_ERR_IO;
    if (stat(p, &s) != 0) return BRIDGE_ERR_NOENT;
    st->type = S_ISDIR(s.st_mode) ? BRIDGE_TYPE_DIR : BRIDGE_TYPE_FILE;
    st->size = S_ISDIR(s.st_mode) ? 0 : (uint64_t)s.st_size;
    return 0;
}

static int fs_list(void *ctx, const char *path, BridgeListFn fn, void *user) {
    char p[2048], q[4096];
    if (host_path(ctx, path, p, sizeof(p))) return BRIDGE_ERR_IO;
    DIR *d = opendir(p);
    if (!d) return BRIDGE_ERR_NOENT;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        BridgeEntry e;
        struct stat s;
        memset(&e, 0, sizeof(e));
        snprintf(q, sizeof(q), "%s/%s", p, de->d_name);
        if (stat(q, &s) != 0) continue;
        e.type = S_ISDIR(s.st_mode) ? BRIDGE_TYPE_DIR : BRIDGE_TYPE_FILE;
        e.size = S_ISDIR(s.st_mode) ? 0 : (uint64_t)s.st_size;
        snprintf(e.name, sizeof(e.name), "%s", de->d_name);
        if (fn(user, &e)) break;
    }
    closedir(d);
    return 0;
}

static int fs_read(void *ctx, const char *path, uint64_t offset, uint8_t *dst, uint32_t len) {
    char p[2048];
    if (host_path(ctx, path, p, sizeof(p))) return BRIDGE_ERR_IO;
    int fd = open(p, O_RDONLY);
    if (fd < 0) return BRIDGE_ERR_NOENT;
    ssize_t n = pread(fd, dst, len, (off_t)offset);
    close(fd);
    return n < 0 ? BRIDGE_ERR_IO : (int)n;
}

FakePsp *fake_psp_new(const char *root) {
    FakePsp *f = calloc(1, sizeof(FakePsp));
    if (!f) return NULL;
    if (root) {
        snprintf(f->root, sizeof(f->root), "%s", root);
        f->hasRoot = 1;
    }
    f->fs.stat = fs_stat;
    f->fs.list = fs_list;
    f->fs.read = fs_read;
    f->fs.ctx = f;
    return f;
}

void fake_psp_free(FakePsp *f) {
    free(f);
}

unsigned fake_psp_requests(const FakePsp *f) {
    return f->requests;
}

static void answer(FakePsp *f, const BridgeHeader *resp) {
    bridge_put_header(f->out, resp);
    f->outHdr = BRIDGE_HEADER_SIZE;
    f->outPayload = (int)resp->len;
    if (resp->len) memcpy(f->out + BRIDGE_HEADER_SIZE, f->payload, resp->len);
}

static void serve(FakePsp *f) {
    BridgeHeader resp;
    f->requests++;
    bridge_handle(&f->req, f->payload, f->requests, &f->fs, &resp);
    answer(f, &resp);
}

static int fake_write(void *ctx, const uint8_t *data, int len) {
    FakePsp *f = ctx;
    if (f->outHdr || f->outPayload) return -1; /* previous answer not read */
    if (f->wantPayload) {
        /* the driver asks for exactly req.len bytes */
        if ((uint32_t)len != f->req.len) return -1;
        memcpy(f->payload, data, len);
        f->wantPayload = 0;
        serve(f);
        return len;
    }
    /* header transfer: the driver receives 16 bytes; anything else is
       answered as not recognised */
    BridgeHeader resp;
    memset(&f->req, 0, sizeof(f->req));
    int err = BRIDGE_ERR_MAGIC;
    if (len == BRIDGE_HEADER_SIZE) {
        bridge_get_header(data, &f->req);
        err = bridge_check_request(&f->req);
    }
    if (err) {
        bridge_error_response(&f->req, err, &resp);
        answer(f, &resp);
    } else if (f->req.len) {
        f->wantPayload = 1;
    } else {
        serve(f);
    }
    return len;
}

static int fake_read(void *ctx, uint8_t *data, int len) {
    FakePsp *f = ctx;
    if (f->outHdr) {
        if (len != f->outHdr) return -1;
        memcpy(data, f->out, len);
        f->outHdr = 0;
        return len;
    }
    if (f->outPayload) {
        if (len != f->outPayload) return -1;
        memcpy(data, f->out + BRIDGE_HEADER_SIZE, len);
        f->outPayload = 0;
        return len;
    }
    return -1; /* nothing to read: a real read would time out */
}

BridgeTransport fake_psp_transport(FakePsp *f) {
    BridgeTransport t = { fake_write, fake_read, f };
    return t;
}
