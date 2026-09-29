/*
 * pspbridged — PSP Bridge step 2: serves the PSP's ISOs to PPSSPP over HTTP.
 *
 *   pspbridged [--port 8765] [--fake DIR] [--iso-dir ms0:/ISO]
 *
 *   GET /status             {"connected":..., "protocol":...}
 *   GET /iso                JSON list: name, size, title, id, url
 *   GET|HEAD /iso/<name>    the ISO, with Range support (PPSSPP "Remote ISO")
 *
 * Listens on 127.0.0.1 only. The PSP is reached through the file channel
 * (libusb), or a local folder playing ms0:/ with --fake (tests). USB access
 * is serialised; ISO reads go through a cache of 64 KB blocks. Opens no
 * window.
 */
#include "bridge_client.h"
#include "bridge_usb.h"
#include "fake_psp.h"
#include "iso_info.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 3000
#define BLOCK BRIDGE_MAX_PAYLOAD
#define NBLOCKS 64 /* 4 MB of cache */

static const char *g_isoDir = "ms0:/ISO";
static const char *g_fakeRoot = NULL;
static int g_verbose = 0;

/* --- link to the PSP ---------------------------------------------------------- */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int up;
    FakePsp *f;
    BridgeUsb *u;
    BridgeClient c;
    BridgeHello hello;
    char error[256];
} L;

typedef struct {
    int valid;
    char path[BRIDGE_PATH_MAX];
    uint64_t index;
    uint32_t len;
    uint64_t used;
    uint8_t *data;
} Block;
static Block g_blocks[NBLOCKS];
static uint64_t g_tick = 0;

static void logf_(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void link_down_locked(void) {
    if (L.f) fake_psp_free(L.f);
    if (L.u) bridge_usb_close(L.u);
    L.f = NULL;
    L.u = NULL;
    L.up = 0;
    for (int i = 0; i < NBLOCKS; i++) g_blocks[i].valid = 0;
}

static int link_up_locked(void) {
    if (L.up) return 1;
    if (g_fakeRoot) {
        L.f = fake_psp_new(g_fakeRoot);
        bridge_client_init(&L.c, fake_psp_transport(L.f));
    } else {
        L.u = bridge_usb_connect(TIMEOUT_MS, &L.c, &L.hello, L.error, sizeof(L.error));
        if (!L.u) return 0;
    }
    int st = L.u ? BRIDGE_OK : bridge_hello(&L.c, &L.hello);
    if (st != BRIDGE_OK || L.hello.version < 2) {
        if (st != BRIDGE_OK)
            snprintf(L.error, sizeof(L.error), "HELLO: %s", bridge_strerror(st));
        else
            snprintf(L.error, sizeof(L.error), "PSP Bridge too old (protocol %u)", L.hello.version);
        link_down_locked();
        return 0;
    }
    L.up = 1;
    L.error[0] = 0;
    fprintf(stderr, "pspbridged: PSP connected (%s, protocol %u)\n", L.hello.name, L.hello.version);
    return 1;
}

/* After a failed call: a transfer error means the cable or the PSP went away */
static int check_locked(int st) {
    if (st == BRIDGE_CLIENT_ERR_IO || st == BRIDGE_CLIENT_ERR_REPLY) {
        fprintf(stderr, "pspbridged: link lost (%s)\n", bridge_strerror(st));
        snprintf(L.error, sizeof(L.error), "%s", bridge_strerror(st));
        link_down_locked();
    }
    return st;
}

static int psp_stat(const char *path, BridgeStat *st) {
    pthread_mutex_lock(&g_lock);
    int r = link_up_locked() ? check_locked(bridge_stat(&L.c, path, st)) : BRIDGE_CLIENT_ERR_IO;
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* One cached block of `path`; copies what the caller wants out of it while
   the lock is held. Returns bytes copied (0 at end of file) or < 0. */
static int read_block(const char *path, uint64_t offset, uint8_t *dst, uint32_t want) {
    uint64_t index = offset / BLOCK;
    uint32_t inBlock = (uint32_t)(offset % BLOCK);
    int r;
    pthread_mutex_lock(&g_lock);
    Block *b = NULL, *victim = &g_blocks[0];
    for (int i = 0; i < NBLOCKS; i++) {
        Block *c = &g_blocks[i];
        if (c->valid && c->index == index && !strcmp(c->path, path)) { b = c; break; }
        /* evict an empty slot first, else the least recently used */
        if (victim->valid && (!c->valid || c->used < victim->used)) victim = c;
    }
    if (!b) {
        if (!link_up_locked()) { r = BRIDGE_CLIENT_ERR_IO; goto out; }
        if (!victim->data) victim->data = malloc(BLOCK);
        if (!victim->data) { r = BRIDGE_CLIENT_ERR_IO; goto out; }
        uint32_t got = 0;
        victim->valid = 0;
        r = check_locked(bridge_read(&L.c, path, index * BLOCK, BLOCK, victim->data, &got));
        if (r != BRIDGE_OK) goto out;
        b = victim;
        b->valid = 1;
        snprintf(b->path, sizeof(b->path), "%s", path);
        b->index = index;
        b->len = got;
    }
    b->used = ++g_tick;
    if (inBlock >= b->len) { r = 0; goto out; }
    r = (int)(b->len - inBlock < want ? b->len - inBlock : want);
    memcpy(dst, b->data + inBlock, (size_t)r);
out:
    pthread_mutex_unlock(&g_lock);
    return r;
}

static int iso_read(void *ctx, uint64_t offset, uint8_t *dst, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        int n = read_block(ctx, offset + done, dst + done, len - done);
        if (n <= 0) return done ? (int)done : n;
        done += (uint32_t)n;
    }
    return (int)done;
}

/* --- ISO list ------------------------------------------------------------------ */

typedef struct {
    char name[256];
    uint64_t size;
    int infoDone;
    IsoInfo info;
} IsoEntry;
static IsoEntry g_isos[512];
static int g_nIsos = 0;
static pthread_mutex_t g_listLock = PTHREAD_MUTEX_INITIALIZER;

static int has_iso_ext(const char *n) {
    size_t l = strlen(n);
    return l > 4 && !strcasecmp(n + l - 4, ".iso");
}

typedef struct { IsoEntry *list; int n; } ListBuild;
static int collect(void *u, const BridgeEntry *e) {
    ListBuild *lb = u;
    if (e->type != BRIDGE_TYPE_FILE || !has_iso_ext(e->name) || e->name[0] == '.') return 0;
    if (lb->n >= (int)(sizeof(g_isos) / sizeof(g_isos[0]))) return 1;
    IsoEntry *it = &lb->list[lb->n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->name, sizeof(it->name), "%s", e->name);
    it->size = e->size;
    return 0;
}

static int cmp_iso(const void *a, const void *b) {
    return strcasecmp(((const IsoEntry *)a)->name, ((const IsoEntry *)b)->name);
}

/* Refreshes g_isos (keeps titles already read for unchanged files) */
static int refresh_isos(void) {
    static IsoEntry fresh[512];
    ListBuild lb = { fresh, 0 };
    pthread_mutex_lock(&g_lock);
    int st = link_up_locked() ? check_locked(bridge_list(&L.c, g_isoDir, collect, &lb)) : BRIDGE_CLIENT_ERR_IO;
    pthread_mutex_unlock(&g_lock);
    if (st != BRIDGE_OK) return st;
    qsort(fresh, lb.n, sizeof(IsoEntry), cmp_iso);
    pthread_mutex_lock(&g_listLock);
    for (int i = 0; i < lb.n; i++)
        for (int j = 0; j < g_nIsos; j++)
            if (!strcmp(fresh[i].name, g_isos[j].name) && fresh[i].size == g_isos[j].size) {
                fresh[i].infoDone = g_isos[j].infoDone;
                fresh[i].info = g_isos[j].info;
            }
    memcpy(g_isos, fresh, sizeof(IsoEntry) * lb.n);
    g_nIsos = lb.n;
    pthread_mutex_unlock(&g_listLock);

    /* titles: a few sectors per ISO, read once */
    for (int i = 0; i < lb.n; i++) {
        pthread_mutex_lock(&g_listLock);
        int done = g_isos[i].infoDone;
        char path[BRIDGE_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", g_isoDir, g_isos[i].name);
        pthread_mutex_unlock(&g_listLock);
        if (done) continue;
        IsoInfo info;
        int ok = iso_info_read(iso_read, path, &info) == 0;
        pthread_mutex_lock(&g_listLock);
        if (i < g_nIsos) {
            g_isos[i].infoDone = 1;
            if (ok) g_isos[i].info = info;
        }
        pthread_mutex_unlock(&g_listLock);
    }
    return BRIDGE_OK;
}

/* --- HTTP ------------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t len, cap;
} Str;

static void str_addf(Str *s, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(s->buf ? s->buf + s->len : NULL, s->buf ? s->cap - s->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if (s->buf && s->len + (size_t)n < s->cap) { s->len += (size_t)n; return; }
        s->cap = (s->cap + (size_t)n + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
}

static void str_add_json(Str *s, const char *v) {
    str_addf(s, "\"");
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p == '"' || *p == '\\') str_addf(s, "\\%c", *p);
        else if (*p < 0x20) str_addf(s, "\\u%04x", *p);
        else str_addf(s, "%c", *p);
    }
    str_addf(s, "\"");
}

static void url_encode(Str *s, const char *v) {
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p)) str_addf(s, "%c", *p);
        else str_addf(s, "%%%02X", *p);
    }
}

static int url_decode(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in; in++) {
        int c = (unsigned char)*in;
        if (c == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char hex[3] = { in[1], in[2], 0 };
            c = (int)strtol(hex, NULL, 16);
            in += 2;
        }
        if (c == 0 || n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}

static int send_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_simple(int fd, int code, const char *reason, const char *type, const char *body,
                       int head, int keep) {
    char hdr[512];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Connection: %s\r\n\r\n",
                     code, reason, type, blen, keep ? "keep-alive" : "close");
    if (send_all(fd, hdr, (size_t)n)) return -1;
    return (head || !blen) ? 0 : send_all(fd, body, blen);
}

static int serve_status(int fd, int head, int keep) {
    Str s = { 0 };
    pthread_mutex_lock(&g_lock);
    int up = link_up_locked();
    str_addf(&s, "{\"connected\":%s,\"protocol\":%u,\"requests\":%u,\"fake\":%s,\"error\":",
             up ? "true" : "false", up ? L.hello.version : 0, up ? L.hello.requests : 0,
             g_fakeRoot ? "true" : "false");
    str_add_json(&s, up ? "" : L.error);
    str_addf(&s, "}\n");
    pthread_mutex_unlock(&g_lock);
    int r = send_simple(fd, 200, "OK", "application/json", s.buf, head, keep);
    free(s.buf);
    return r;
}

static int serve_list(int fd, int head, int keep) {
    int st = refresh_isos();
    if (st != BRIDGE_OK) {
        Str s = { 0 };
        str_addf(&s, "{\"error\":");
        pthread_mutex_lock(&g_lock);
        str_add_json(&s, st == BRIDGE_ERR_NOENT ? "no ISO folder on the PSP"
                         : L.error[0] ? L.error : bridge_strerror(st));
        pthread_mutex_unlock(&g_lock);
        str_addf(&s, "}\n");
        int r = send_simple(fd, st == BRIDGE_ERR_NOENT ? 404 : 503,
                            st == BRIDGE_ERR_NOENT ? "Not Found" : "Service Unavailable",
                            "application/json", s.buf, head, keep);
        free(s.buf);
        return r;
    }
    Str s = { 0 };
    str_addf(&s, "[");
    pthread_mutex_lock(&g_listLock);
    for (int i = 0; i < g_nIsos; i++) {
        IsoEntry *it = &g_isos[i];
        str_addf(&s, "%s\n {\"name\":", i ? "," : "");
        str_add_json(&s, it->name);
        str_addf(&s, ",\"size\":%llu,\"title\":", (unsigned long long)it->size);
        str_add_json(&s, it->info.title);
        str_addf(&s, ",\"id\":");
        str_add_json(&s, it->info.id);
        str_addf(&s, ",\"url\":\"/iso/");
        url_encode(&s, it->name);
        str_addf(&s, "\"}");
    }
    pthread_mutex_unlock(&g_listLock);
    str_addf(&s, "\n]\n");
    int r = send_simple(fd, 200, "OK", "application/json", s.buf, head, keep);
    free(s.buf);
    return r;
}

/* "bytes=a-b", "bytes=a-", "bytes=-n" -> [first, last]; 0 ok, -1 invalid,
   -2 unsatisfiable */
static int parse_range(const char *v, uint64_t size, uint64_t *first, uint64_t *last) {
    while (*v == ' ') v++;
    if (strncasecmp(v, "bytes=", 6)) return -1;
    v += 6;
    if (strchr(v, ',')) return -1; /* one range only */
    char *end;
    if (*v == '-') {
        unsigned long long n = strtoull(v + 1, &end, 10);
        if (end == v + 1 || n == 0) return -2;
        if (size == 0) return -2;
        *first = n >= size ? 0 : size - n;
        *last = size - 1;
        return 0;
    }
    unsigned long long a = strtoull(v, &end, 10);
    if (end == v || *end != '-') return -1;
    const char *b = end + 1;
    unsigned long long z = size ? size - 1 : 0;
    if (*b) {
        z = strtoull(b, &end, 10);
        if (end == b) return -1;
        if (z >= size) z = size - 1;
    }
    if (a >= size || a > z) return -2;
    *first = a;
    *last = z;
    return 0;
}

static int serve_iso(int fd, const char *rawName, const char *range, int head, int keep) {
    char name[256], path[BRIDGE_PATH_MAX];
    if (url_decode(rawName, name, sizeof(name)) || !name[0] || strchr(name, '/') || !has_iso_ext(name))
        return send_simple(fd, 404, "Not Found", "text/plain", "not found\n", head, keep);
    snprintf(path, sizeof(path), "%s/%s", g_isoDir, name);
    if (!bridge_path_ok(path))
        return send_simple(fd, 404, "Not Found", "text/plain", "not found\n", head, keep);

    BridgeStat st;
    int r = psp_stat(path, &st);
    if (r == BRIDGE_ERR_NOENT || (r == BRIDGE_OK && st.type != BRIDGE_TYPE_FILE))
        return send_simple(fd, 404, "Not Found", "text/plain", "not found\n", head, keep);
    if (r != BRIDGE_OK)
        return send_simple(fd, 503, "Service Unavailable", "text/plain", "PSP not connected\n", head, keep);

    uint64_t first = 0, last = st.size ? st.size - 1 : 0;
    int partial = 0;
    if (range) {
        int pr = parse_range(range, st.size, &first, &last);
        if (pr == -2) {
            char hdr[256];
            int n = snprintf(hdr, sizeof(hdr),
                             "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\n"
                             "Content-Length: 0\r\nConnection: %s\r\n\r\n",
                             (unsigned long long)st.size, keep ? "keep-alive" : "close");
            return send_all(fd, hdr, (size_t)n);
        }
        partial = (pr == 0);
        if (pr == -1) { first = 0; last = st.size ? st.size - 1 : 0; }
    }
    uint64_t length = st.size ? last - first + 1 : 0;

    char hdr[512];
    int n;
    if (partial)
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream\r\n"
                     "Accept-Ranges: bytes\r\nContent-Range: bytes %llu-%llu/%llu\r\n"
                     "Content-Length: %llu\r\nConnection: %s\r\n\r\n",
                     (unsigned long long)first, (unsigned long long)last, (unsigned long long)st.size,
                     (unsigned long long)length, keep ? "keep-alive" : "close");
    else
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Accept-Ranges: bytes\r\nContent-Length: %llu\r\nConnection: %s\r\n\r\n",
                     (unsigned long long)length, keep ? "keep-alive" : "close");
    if (send_all(fd, hdr, (size_t)n)) return -1;
    if (head) return 0;

    logf_("GET %s %llu-%llu\n", name, (unsigned long long)first, (unsigned long long)last);
    static __thread uint8_t chunk[BLOCK];
    uint64_t pos = first, end = first + length;
    while (pos < end) {
        uint32_t want = (uint32_t)((end - pos) < BLOCK ? (end - pos) : BLOCK);
        int got = iso_read(path, pos, chunk, want);
        if (got <= 0) return -1; /* PSP gone mid-transfer: drop the connection */
        if (send_all(fd, chunk, (size_t)got)) return -1;
        pos += (uint64_t)got;
    }
    return 0;
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    char buf[8192];
    size_t have = 0;
    for (;;) {
        /* read one request head */
        char *eoh = NULL;
        while (!(eoh = (have ? strstr(buf, "\r\n\r\n") : NULL))) {
            if (have >= sizeof(buf) - 1) goto done;
            ssize_t n = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
            if (n <= 0) goto done;
            have += (size_t)n;
            buf[have] = 0;
        }
        size_t headLen = (size_t)(eoh - buf) + 4;
        *eoh = 0;

        char method[8] = "", target[2048] = "", version[16] = "";
        if (sscanf(buf, "%7s %2047s %15s", method, target, version) != 3) goto done;
        const char *range = NULL;
        int keep = !strcmp(version, "HTTP/1.1");
        char rangeBuf[128];
        for (char *line = strstr(buf, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
            char *h = line + 2;
            if (!strncasecmp(h, "Range:", 6)) {
                snprintf(rangeBuf, sizeof(rangeBuf), "%.*s", (int)strcspn(h + 6, "\r"), h + 6);
                range = rangeBuf;
            } else if (!strncasecmp(h, "Connection:", 11)) {
                const char *v = h + 11;
                while (*v == ' ') v++;
                if (!strncasecmp(v, "close", 5)) keep = 0;
                else if (!strncasecmp(v, "keep-alive", 10)) keep = 1;
            }
        }
        int head = !strcmp(method, "HEAD");
        int r;
        if (strcmp(method, "GET") && !head) {
            r = send_simple(fd, 405, "Method Not Allowed", "text/plain", "GET or HEAD\n", 0, 0);
            keep = 0;
        } else {
            char *q = strchr(target, '?');
            if (q) *q = 0;
            if (!strcmp(target, "/status")) r = serve_status(fd, head, keep);
            else if (!strcmp(target, "/iso") || !strcmp(target, "/iso/")) r = serve_list(fd, head, keep);
            else if (!strncmp(target, "/iso/", 5)) r = serve_iso(fd, target + 5, range, head, keep);
            else r = send_simple(fd, 404, "Not Found", "text/plain", "not found\n", head, keep);
        }
        if (r || !keep) goto done;
        /* keep what followed this request (pipelining) */
        memmove(buf, buf + headLen, have - headLen);
        have -= headLen;
        buf[have] = 0;
    }
done:
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int port = 8765;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fake") && i + 1 < argc) g_fakeRoot = argv[++i];
        else if (!strcmp(argv[i], "--iso-dir") && i + 1 < argc) g_isoDir = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) g_verbose = 1;
        else {
            fprintf(stderr, "usage: pspbridged [--port N] [--fake DIR] [--iso-dir ms0:/ISO] [--verbose]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 16)) {
        perror("pspbridged: bind");
        return 1;
    }
    fprintf(stderr, "pspbridged: http://127.0.0.1:%d/iso (%s)\n", port,
            g_fakeRoot ? "fake PSP" : "PSP over USB");
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            perror("pspbridged: accept");
            return 1;
        }
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)c);
        pthread_detach(t);
    }
}
