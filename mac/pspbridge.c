/*
 * pspbridge — test tool for PSP Bridge, step 1 (composite device).
 *
 *   pspbridge info               descriptors as macOS sees them
 *   pspbridge hello              file channel: HELLO
 *   pspbridge echo [N]           N echoes of various sizes, data checked
 *   pspbridge bench [S] [SIZE]   echo throughput for S seconds
 *   pspbridge pad [S]            gamepad reports (HID, interface 0)
 *   pspbridge check [S]          the step-1 test: gamepad and file channel
 *                                together for S seconds, with a verdict
 *   pspbridge ls [PATH]          list a Memory Stick folder (default ms0:/ISO)
 *   pspbridge readbench FILE [S] read FILE in 64 KB requests for S seconds
 *
 * --fake runs hello/echo/bench against the in-process fake PSP.
 */
#include "bridge_client.h"
#include "bridge_usb.h"
#include "fake_psp.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMEOUT_MS 2000

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* --- file channel ---------------------------------------------------------- */

typedef struct {
    int fake;
    FakePsp *f;
    BridgeUsb *u;
    BridgeClient c;
} Channel;

static int channel_open(Channel *ch, int fake) {
    char err[256];
    memset(ch, 0, sizeof(*ch));
    ch->fake = fake;
    if (fake) {
        ch->f = fake_psp_new(NULL);
        bridge_client_init(&ch->c, fake_psp_transport(ch->f));
        return 0;
    }
    ch->u = bridge_usb_open(TIMEOUT_MS, err, sizeof(err));
    if (!ch->u) {
        fprintf(stderr, "file channel: %s\n", err);
        return -1;
    }
    bridge_client_init(&ch->c, bridge_usb_transport(ch->u));
    return 0;
}

static void channel_close(Channel *ch) {
    if (ch->f) fake_psp_free(ch->f);
    if (ch->u) bridge_usb_close(ch->u);
}

static const uint32_t kSizes[] = { 0, 1, 15, 16, 17, 63, 64, 65, 511, 512, 513,
                                   1024, 4096, 16384, 32768, 65535, 65536 };
#define NSIZES (sizeof(kSizes) / sizeof(kSizes[0]))

static int cmd_hello(Channel *ch) {
    BridgeHello h;
    int st = bridge_hello(&ch->c, &h);
    if (st != BRIDGE_OK) {
        printf("HELLO failed: %s (%d)\n", bridge_strerror(st), st);
        return 1;
    }
    printf("HELLO ok: \"%s\", protocol %u, max payload %u, requests served %u\n",
           h.name, h.version, h.maxPayload, h.requests);
    return 0;
}

static int cmd_echo(Channel *ch, int count) {
    int failures = 0;
    for (int i = 0; i < count; i++) {
        uint32_t len = kSizes[i % NSIZES];
        int st = bridge_echo_check(&ch->c, len, (uint32_t)i);
        if (st != BRIDGE_OK) {
            printf("echo %d (%u bytes) FAILED: %s (%d)\n", i, len, bridge_strerror(st), st);
            if (++failures >= 5) break;
        }
    }
    printf("ECHO %s: %d/%d\n", failures ? "FAILED" : "OK", count - failures, count);
    return failures ? 1 : 0;
}

static int cmd_bench(Channel *ch, double seconds, uint32_t size) {
    double t0 = now_s(), t;
    uint64_t bytes = 0;
    int n = 0;
    while ((t = now_s()) - t0 < seconds) {
        int st = bridge_echo_check(&ch->c, size, (uint32_t)n);
        if (st != BRIDGE_OK) {
            printf("bench: echo %d FAILED: %s\n", n, bridge_strerror(st));
            return 1;
        }
        bytes += size;
        n++;
    }
    t = now_s() - t0;
    printf("BENCH %u-byte echoes: %d in %.1f s, %.2f MB/s each way, %.2f ms per request\n",
           size, n, t, bytes / t / 1e6, t * 1000.0 / (n ? n : 1));
    return 0;
}

static int print_entry(void *u, const BridgeEntry *e) {
    (*(int *)u)++;
    if (e->type == BRIDGE_TYPE_DIR) printf("  %-12s  %s/\n", "", e->name);
    else printf("  %12llu  %s\n", (unsigned long long)e->size, e->name);
    return 0;
}

static int cmd_ls(Channel *ch, const char *path) {
    int n = 0;
    int st = bridge_list(&ch->c, path, print_entry, &n);
    if (st != BRIDGE_OK) {
        printf("LS %s failed: %s (%d)\n", path, bridge_strerror(st), st);
        return 1;
    }
    printf("LS %s: %d entries\n", path, n);
    return 0;
}

static int cmd_readbench(Channel *ch, const char *path, double seconds) {
    BridgeStat s;
    int st = bridge_stat(&ch->c, path, &s);
    if (st != BRIDGE_OK || s.type != BRIDGE_TYPE_FILE) {
        printf("READBENCH: %s: %s\n", path, st != BRIDGE_OK ? bridge_strerror(st) : "not a file");
        return 1;
    }
    uint8_t *buf = malloc(BRIDGE_MAX_PAYLOAD);
    uint64_t off = 0, bytes = 0;
    double t0 = now_s(), t;
    int n = 0;
    while ((t = now_s()) - t0 < seconds && off < s.size) {
        uint32_t got = 0;
        st = bridge_read(&ch->c, path, off, BRIDGE_MAX_PAYLOAD, buf, &got);
        if (st != BRIDGE_OK || got == 0) break;
        off += got;
        bytes += got;
        n++;
    }
    free(buf);
    t = now_s() - t0;
    if (st != BRIDGE_OK) printf("READBENCH: read failed at %llu: %s\n", (unsigned long long)off, bridge_strerror(st));
    printf("READBENCH %s: %.1f MB in %.1f s = %.2f MB/s (%d requests; a UMD reads ~1.6 MB/s)\n",
           path, bytes / 1e6, t, bytes / t / 1e6, n);
    return st == BRIDGE_OK ? 0 : 1;
}

/* --- gamepad (HID) ----------------------------------------------------------- */

static _Atomic int padReports = 0;
static _Atomic int padSeen = 0;
static int padVerbose = 1;
static uint8_t padLast[8];
static uint8_t padReport[64];

static void on_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex len) {
    atomic_fetch_add(&padReports, 1);
    if (len < 6) return;
    if (padVerbose && memcmp(padLast, report, 6) != 0) {
        memcpy(padLast, report, 6);
        unsigned buttons = report[4] | (report[5] << 8);
        printf("  pad: stick X %3u Y %3u  dpad %3u/%3u  buttons %04X\n",
               report[0], report[1], report[2], report[3], buttons);
        fflush(stdout);
    }
}

static void on_match(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev) {
    atomic_store(&padSeen, 1);
    IOHIDDeviceRegisterInputReportCallback(dev, padReport, sizeof(padReport), on_report, NULL);
}

static IOHIDManagerRef pad_open(void) {
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = PSP_VID, pid = PSP_BRIDGE_PID;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *vals[] = { v, p };
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(m, match);
    IOHIDManagerRegisterDeviceMatchingCallback(m, on_match, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) fprintf(stderr, "IOHIDManagerOpen: 0x%x\n", r);
    CFRelease(match);
    CFRelease(v);
    CFRelease(p);
    return m;
}

static void pad_run(double seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
}

static int cmd_pad(double seconds) {
    IOHIDManagerRef m = pad_open();
    printf("Gamepad for %.0f s: move the stick, press buttons...\n", seconds);
    pad_run(seconds);
    int n = atomic_load(&padReports);
    printf("PAD %s: %d reports (%.0f/s)\n", atomic_load(&padSeen) ? (n ? "OK" : "SEEN, NO REPORT") : "NOT FOUND",
           n, n / seconds);
    IOHIDManagerClose(m, kIOHIDOptionsTypeNone);
    CFRelease(m);
    return (atomic_load(&padSeen) && n) ? 0 : 1;
}

/* --- check: both at once ----------------------------------------------------- */

typedef struct {
    Channel *ch;
    double seconds;
    _Atomic int done;
    int ok, failed;
    uint64_t bytes;
    char firstError[128];
} EchoLoad;

static void *echo_thread(void *arg) {
    EchoLoad *l = arg;
    double t0 = now_s();
    int i = 0;
    while (now_s() - t0 < l->seconds) {
        uint32_t len = kSizes[i % NSIZES];
        int st = bridge_echo_check(&l->ch->c, len, (uint32_t)i++);
        if (st == BRIDGE_OK) {
            l->ok++;
            l->bytes += len;
        } else {
            if (!l->failed)
                snprintf(l->firstError, sizeof(l->firstError), "%u bytes: %s", len, bridge_strerror(st));
            if (++l->failed >= 20) break;
        }
    }
    atomic_store(&l->done, 1);
    return NULL;
}

static int cmd_check(double seconds) {
    printf("== descriptors\n");
    if (bridge_usb_describe() != 0) return 1;

    printf("== file channel\n");
    Channel ch;
    int chanOk = (channel_open(&ch, 0) == 0) && (cmd_hello(&ch) == 0);

    printf("== gamepad + file channel together, %.0f s: move the stick, press buttons\n", seconds);
    IOHIDManagerRef m = pad_open();
    padVerbose = 1;
    EchoLoad load;
    memset(&load, 0, sizeof(load));
    load.ch = &ch;
    load.seconds = seconds;
    pthread_t th;
    if (chanOk) pthread_create(&th, NULL, echo_thread, &load);
    double t0 = now_s();
    while (now_s() - t0 < seconds + 0.5 && !(chanOk && atomic_load(&load.done) && now_s() - t0 >= seconds))
        pad_run(0.1);
    if (chanOk) pthread_join(th, NULL);
    int reports = atomic_load(&padReports);
    IOHIDManagerClose(m, kIOHIDOptionsTypeNone);
    CFRelease(m);

    printf("== verdict\n");
    int padOk = atomic_load(&padSeen) && reports > 0;
    printf("  gamepad:      %s (%d reports, %.0f/s)\n", padOk ? "OK" : "NOT WORKING", reports, reports / seconds);
    if (chanOk) {
        printf("  file channel: %s (%d echoes ok, %d failed, %.2f MB/s each way)%s%s\n",
               load.failed ? "ERRORS" : "OK", load.ok, load.failed, load.bytes / seconds / 1e6,
               load.failed ? ", first: " : "", load.firstError);
    } else {
        printf("  file channel: NOT WORKING\n");
    }
    int ok = padOk && chanOk && !load.failed;
    printf("STEP 1 %s\n", ok ? "PASSED" : "FAILED");
    if (chanOk) channel_close(&ch);
    return ok ? 0 : 1;
}

/* --------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
            "usage: pspbridge [--fake] info | hello | echo [N] | bench [SECONDS] [SIZE] |\n"
            "                          pad [SECONDS] | check [SECONDS] | ls [PATH] |\n"
            "                          readbench FILE [SECONDS]\n");
}

int main(int argc, char **argv) {
    int fake = 0;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "--fake")) {
        fake = 1;
        a++;
    }
    if (a >= argc) {
        usage();
        return 2;
    }
    const char *cmd = argv[a++];
    const char *arg1 = a < argc ? argv[a] : NULL;
    const char *arg2 = a + 1 < argc ? argv[a + 1] : NULL;

    if (!strcmp(cmd, "info")) return bridge_usb_describe() ? 1 : 0;
    if (!strcmp(cmd, "pad")) return cmd_pad(arg1 ? atof(arg1) : 10);
    if (!strcmp(cmd, "check")) return cmd_check(arg1 ? atof(arg1) : 15);

    Channel ch;
    int ret;
    if (!strcmp(cmd, "hello") || !strcmp(cmd, "echo") || !strcmp(cmd, "bench") ||
        !strcmp(cmd, "ls") || (!strcmp(cmd, "readbench") && arg1)) {
        if (channel_open(&ch, fake) != 0) return 1;
        if (!strcmp(cmd, "hello")) ret = cmd_hello(&ch);
        else if (!strcmp(cmd, "ls")) ret = cmd_ls(&ch, arg1 ? arg1 : "ms0:/ISO");
        else if (!strcmp(cmd, "readbench")) ret = cmd_readbench(&ch, arg1, arg2 ? atof(arg2) : 10);
        else if (!strcmp(cmd, "echo")) ret = cmd_echo(&ch, arg1 ? atoi(arg1) : 200);
        else ret = cmd_bench(&ch, arg1 ? atof(arg1) : 5, arg2 ? (uint32_t)atoi(arg2) : 65536);
        channel_close(&ch);
        return ret;
    }
    usage();
    return 2;
}
