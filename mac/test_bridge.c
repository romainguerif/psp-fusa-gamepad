/* Offline tests of the file channel: Mac client against the fake PSP, which
   runs the driver's own protocol code (src/common/bridge_proto.c). */
#include "bridge_client.h"
#include "fake_psp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static int raw_write(BridgeTransport t, const BridgeHeader *h) {
    uint8_t b[BRIDGE_HEADER_SIZE];
    bridge_put_header(b, h);
    return t.write(t.ctx, b, BRIDGE_HEADER_SIZE);
}

static int raw_read(BridgeTransport t, BridgeHeader *h) {
    uint8_t b[BRIDGE_HEADER_SIZE];
    if (t.read(t.ctx, b, BRIDGE_HEADER_SIZE) != BRIDGE_HEADER_SIZE) return -1;
    bridge_get_header(b, h);
    return 0;
}

typedef struct { int files, dirs, bigSeen; uint64_t bigSize; } ListCount;
static int count_entry(void *u, const BridgeEntry *e) {
    ListCount *lc = u;
    if (e->type == BRIDGE_TYPE_DIR) lc->dirs++; else lc->files++;
    if (!strcmp(e->name, "GAME.iso")) { lc->bigSeen = 1; lc->bigSize = e->size; }
    return 0;
}
static int stop_at_first(void *u, const BridgeEntry *e) { (*(int *)u)++; return 1; }

static void file_tests(void) {
    char root[] = "/tmp/pspbridge-test-XXXXXX";
    char p[512];
    CHECK(mkdtemp(root) != NULL);
    snprintf(p, sizeof(p), "%s/ISO", root); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ISO/sub", root); mkdir(p, 0755);
    /* a 300 KB "ISO" with a known pattern */
    const uint32_t isoSize = 300 * 1024 + 123;
    uint8_t *iso = malloc(isoSize);
    for (uint32_t i = 0; i < isoSize; i++) iso[i] = (uint8_t)(i * 7 + (i >> 11));
    snprintf(p, sizeof(p), "%s/ISO/GAME.iso", root);
    FILE *fp = fopen(p, "wb"); fwrite(iso, 1, isoSize, fp); fclose(fp);
    /* enough long names to need several LIST answers (> 64 KB) */
    for (int i = 0; i < 400; i++) {
        snprintf(p, sizeof(p), "%s/ISO/save_%03d_%0200d.bin", root, i, 0);
        fp = fopen(p, "wb"); fputc(i, fp); fclose(fp);
    }

    FakePsp *f = fake_psp_new(root);
    BridgeClient c;
    bridge_client_init(&c, fake_psp_transport(f));

    BridgeStat st;
    CHECK(bridge_stat(&c, "ms0:/ISO/GAME.iso", &st) == BRIDGE_OK);
    CHECK(st.type == BRIDGE_TYPE_FILE && st.size == isoSize);
    CHECK(bridge_stat(&c, "ms0:/ISO", &st) == BRIDGE_OK && st.type == BRIDGE_TYPE_DIR);
    CHECK(bridge_stat(&c, "ms0:/ISO/none.iso", &st) == BRIDGE_ERR_NOENT);

    ListCount lc = { 0, 0, 0, 0 };
    CHECK(bridge_list(&c, "ms0:/ISO", count_entry, &lc) == BRIDGE_OK);
    CHECK(lc.files == 401 && lc.dirs == 1);
    CHECK(lc.bigSeen && lc.bigSize == isoSize);
    int seen = 0;
    CHECK(bridge_list(&c, "ms0:/ISO", stop_at_first, &seen) == BRIDGE_OK && seen == 1);
    CHECK(bridge_list(&c, "ms0:/NOPE", count_entry, &lc) == BRIDGE_ERR_NOENT);

    /* reads: start, middle across chunk sizes, end (short), past the end */
    uint8_t *buf = malloc(BRIDGE_MAX_PAYLOAD);
    uint32_t got;
    CHECK(bridge_read(&c, "ms0:/ISO/GAME.iso", 0, 2048, buf, &got) == BRIDGE_OK);
    CHECK(got == 2048 && !memcmp(buf, iso, 2048));
    CHECK(bridge_read(&c, "ms0:/ISO/GAME.iso", 100001, BRIDGE_MAX_PAYLOAD, buf, &got) == BRIDGE_OK);
    CHECK(got == BRIDGE_MAX_PAYLOAD && !memcmp(buf, iso + 100001, got));
    CHECK(bridge_read(&c, "ms0:/ISO/GAME.iso", isoSize - 100, 4096, buf, &got) == BRIDGE_OK);
    CHECK(got == 100 && !memcmp(buf, iso + isoSize - 100, 100));
    CHECK(bridge_read(&c, "ms0:/ISO/GAME.iso", isoSize + 5, 4096, buf, &got) == BRIDGE_OK && got == 0);
    CHECK(bridge_read(&c, "ms0:/ISO/none.iso", 0, 16, buf, &got) == BRIDGE_ERR_NOENT);

    /* paths outside the Memory Stick or climbing out are refused */
    const char *bad[] = { "host0:/x", "ms0:", "ms0:/../etc/passwd", "ms0:/ISO/../../x",
                          "ms0:/./ISO", "ms0:/ISO\\x", "/etc/passwd", "ms0:/ISO/.." };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int r = bridge_stat(&c, bad[i], &st);
        if (r != BRIDGE_ERR_PATH) printf("  path %s: %d\n", bad[i], r);
        CHECK(r == BRIDGE_ERR_PATH);
    }
    CHECK(bridge_path_ok("ms0:/PSP/SAVEDATA/ULUS10041/DATA.BIN"));
    CHECK(bridge_path_ok("ms0:/ISO/..hidden.iso"));
    CHECK(bridge_path_ok("ms0:/"));

    /* the channel is still in step after all the errors */
    CHECK(bridge_echo_check(&c, 777, 3) == BRIDGE_OK);

    free(buf);
    free(iso);
    fake_psp_free(f);
    snprintf(p, sizeof(p), "rm -rf '%s'", root);
    CHECK(system(p) == 0);
}

int main(void) {
    /* header layout is little endian, fields at fixed offsets */
    {
        BridgeHeader h = { BRIDGE_REQ_MAGIC, 0x0102, -3, 0x11223344, 0x00010000 }, g;
        uint8_t b[16];
        bridge_put_header(b, &h);
        CHECK(!memcmp(b, "PBRQ", 4));
        CHECK(b[4] == 0x02 && b[5] == 0x01 && b[6] == 0xFD && b[7] == 0xFF);
        CHECK(b[8] == 0x44 && b[11] == 0x11 && b[14] == 0x01);
        bridge_get_header(b, &g);
        CHECK(g.magic == h.magic && g.cmd == h.cmd && g.status == -3 && g.seq == h.seq && g.len == h.len);
    }

    FakePsp *f = fake_psp_new(NULL);
    BridgeTransport t = fake_psp_transport(f);
    BridgeClient c;
    bridge_client_init(&c, t);

    BridgeHello hello;
    CHECK(bridge_hello(&c, &hello) == BRIDGE_OK);
    CHECK(hello.version == BRIDGE_PROTO_VERSION);
    CHECK(hello.maxPayload == BRIDGE_MAX_PAYLOAD);
    CHECK(!strcmp(hello.name, "PSP Bridge"));
    CHECK(hello.requests == 1);

    /* echo: sizes around the USB packet sizes and the limits */
    const uint32_t sizes[] = { 0, 1, 15, 16, 17, 63, 64, 65, 511, 512, 513, 4096, 65535, 65536 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int st = bridge_echo_check(&c, sizes[i], i);
        if (st != BRIDGE_OK) printf("  echo %u: %s\n", sizes[i], bridge_strerror(st));
        CHECK(st == BRIDGE_OK);
    }

    /* the client refuses to send more than the PSP accepts */
    static uint8_t big[BRIDGE_MAX_PAYLOAD + 1];
    CHECK(bridge_call(&c, BRIDGE_CMD_ECHO, big, sizeof(big), NULL, 0, NULL) == BRIDGE_CLIENT_ERR_ARG);

    /* unknown command: answered, no payload, the channel stays in step */
    uint32_t got = 99;
    CHECK(bridge_call(&c, 0x7777, NULL, 0, NULL, 0, &got) == BRIDGE_ERR_UNKNOWN);
    CHECK(got == 0);
    CHECK(bridge_echo_check(&c, 100, 7) == BRIDGE_OK);

    /* a header the PSP doesn't understand gets a "not recognised" answer
       (the driver never waits for a payload it can't size) */
    {
        BridgeHeader bad = { 0xDEADBEEF, BRIDGE_CMD_ECHO, 0, 5, 1000 }, r;
        CHECK(raw_write(t, &bad) == BRIDGE_HEADER_SIZE);
        CHECK(raw_read(t, &r) == 0);
        CHECK(r.magic == BRIDGE_RESP_MAGIC && r.status == BRIDGE_ERR_MAGIC && r.len == 0);
        CHECK(bridge_echo_check(&c, 10, 8) == BRIDGE_OK);
    }
    /* same for a payload announced bigger than the limit */
    {
        BridgeHeader tooBig = { BRIDGE_REQ_MAGIC, BRIDGE_CMD_ECHO, 0, 6, BRIDGE_MAX_PAYLOAD + 1 }, r;
        CHECK(raw_write(t, &tooBig) == BRIDGE_HEADER_SIZE);
        CHECK(raw_read(t, &r) == 0);
        CHECK(r.status == BRIDGE_ERR_TOO_BIG && r.seq == 6 && r.len == 0);
    }

    /* sequence numbers are echoed; a wrong one is caught by the client */
    {
        BridgeHeader req = { BRIDGE_REQ_MAGIC, BRIDGE_CMD_HELLO, 0, 42, 0 }, r;
        CHECK(raw_write(t, &req) == BRIDGE_HEADER_SIZE);
        CHECK(raw_read(t, &r) == 0);
        CHECK(r.seq == 42 && r.cmd == BRIDGE_CMD_HELLO && r.len == BRIDGE_HELLO_SIZE);
        uint8_t skip[BRIDGE_HELLO_SIZE];
        CHECK(t.read(t.ctx, skip, BRIDGE_HELLO_SIZE) == BRIDGE_HELLO_SIZE);
    }

    /* nothing pending: a read "times out" instead of inventing data */
    {
        uint8_t b[16];
        CHECK(t.read(t.ctx, b, 16) < 0);
    }

    CHECK(bridge_hello(&c, &hello) == BRIDGE_OK);
    CHECK(hello.requests == fake_psp_requests(f));

    fake_psp_free(f);

    /* no Memory Stick behind the fake: file commands fail cleanly */
    {
        FakePsp *g = fake_psp_new(NULL);
        BridgeClient d;
        BridgeStat st;
        bridge_client_init(&d, fake_psp_transport(g));
        CHECK(bridge_stat(&d, "ms0:/ISO", &st) == BRIDGE_ERR_IO);
        fake_psp_free(g);
    }
    file_tests();

    printf(failures ? "%d failure(s)\n" : "BRIDGE OK\n", failures);
    return failures ? 1 : 0;
}
