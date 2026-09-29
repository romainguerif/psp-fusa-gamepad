/* PSP Bridge — the PSP's gamepad into PPSSPP, see pad_forward.h */
#include "pad_forward.h"
#include "bridge_usb.h"
#include "ws_client.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* --- report -> PPSSPP messages ------------------------------------------- */

/* FuSa report: 0 stick X, 1 stick Y, 2-3 d-pad axes, 4-5 buttons */
static const struct { int byte; int bit; const char *name; } kButtons[] = {
    { 4, 0x01, "cross" },    { 4, 0x02, "circle" },  { 4, 0x04, "square" },
    { 4, 0x08, "triangle" }, { 4, 0x10, "ltrigger" }, { 4, 0x20, "rtrigger" },
    { 4, 0x40, "home" },     { 4, 0x80, "screen" },  { 5, 0x01, "select" },
    { 5, 0x02, "start" },    { 5, 0x04, "vol_down" }, { 5, 0x08, "vol_up" },
    { 5, 0x10, "up" },       { 5, 0x20, "down" },    { 5, 0x40, "left" },
    { 5, 0x80, "right" },
};
#define NBUTTONS (int)(sizeof(kButtons) / sizeof(kButtons[0]))

/* The PSP stick rests up to ~25 off centre (this one: X 131, Y 151) */
#define DEAD_ZONE 0.25f

float pad_axis(int raw) {
    float v = (raw - 128) / 127.0f;
    if (v > 1) v = 1;
    if (v < -1) v = -1;
    float a = fabsf(v);
    if (a < DEAD_ZONE) return 0;
    /* rescale so that the edge of the dead zone is 0 */
    v = (v > 0 ? 1 : -1) * (a - DEAD_ZONE) / (1 - DEAD_ZONE);
    return roundf(v * 100) / 100; /* 0.01 steps: no flood of tiny changes */
}

static int put(char *out, int outSize, int *used, const char *s) {
    int n = (int)strlen(s) + 1;
    if (*used + n + 1 > outSize) return 0;
    memcpy(out + *used, s, (size_t)n);
    *used += n;
    return 1;
}

int pad_messages(const uint8_t *prev, const uint8_t *cur, char *out, int outSize) {
    static const uint8_t released[PAD_REPORT_SIZE] = { 128, 128, 127, 127, 0, 0, 0, 0 };
    char msg[1024];
    int used = 0, count = 0;
    if (!prev) prev = released;
    if (!cur) cur = released;

    /* buttons: one message with every change */
    int len = snprintf(msg, sizeof(msg), "{\"event\":\"input.buttons.send\",\"buttons\":{");
    int changes = 0;
    for (int i = 0; i < NBUTTONS; i++) {
        int was = (prev[kButtons[i].byte] & kButtons[i].bit) != 0;
        int is = (cur[kButtons[i].byte] & kButtons[i].bit) != 0;
        if (was == is) continue;
        len += snprintf(msg + len, sizeof(msg) - (size_t)len, "%s\"%s\":%s", changes ? "," : "",
                        kButtons[i].name, is ? "true" : "false");
        changes++;
    }
    if (changes) {
        snprintf(msg + len, sizeof(msg) - (size_t)len, "}}");
        if (put(out, outSize, &used, msg)) count++;
    }

    /* left stick */
    float px = pad_axis(prev[0]), py = -pad_axis(prev[1]);
    float cx = pad_axis(cur[0]), cy = -pad_axis(cur[1]);
    if (px != cx || py != cy) {
        snprintf(msg, sizeof(msg), "{\"event\":\"input.analog.send\",\"stick\":\"left\",\"x\":%.2f,\"y\":%.2f}",
                 cx, cy);
        if (put(out, outSize, &used, msg)) count++;
    }
    out[used] = 0;
    return count;
}

/* --- HID reader ----------------------------------------------------------- */

static pthread_mutex_t g_padLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_padCond = PTHREAD_COND_INITIALIZER;
static uint8_t g_pad[PAD_REPORT_SIZE];
static int g_padPresent = 0;
static unsigned g_padSeq = 0;
static uint8_t g_hidBuf[64];

static void on_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type,
                      uint32_t reportID, uint8_t *report, CFIndex len) {
    if (len < 6) return;
    pthread_mutex_lock(&g_padLock);
    if (memcmp(g_pad, report, 6) != 0 || !g_padPresent) {
        memcpy(g_pad, report, len < PAD_REPORT_SIZE ? (size_t)len : PAD_REPORT_SIZE);
        g_padPresent = 1;
        g_padSeq++;
        pthread_cond_signal(&g_padCond);
    }
    pthread_mutex_unlock(&g_padLock);
}

static void on_match(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev) {
    fprintf(stderr, "pspbridged: gamepad found, forwarding it to PPSSPP\n");
    IOHIDDeviceRegisterInputReportCallback(dev, g_hidBuf, sizeof(g_hidBuf), on_report, NULL);
}

static void on_remove(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef dev) {
    pthread_mutex_lock(&g_padLock);
    g_padPresent = 0; /* everything released */
    g_padSeq++;
    pthread_cond_signal(&g_padCond);
    pthread_mutex_unlock(&g_padLock);
}

static CFDictionaryRef match_dict(int vid, int pid) {
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    const void *keys[] = { CFSTR(kIOHIDVendorIDKey), CFSTR(kIOHIDProductIDKey) };
    const void *vals[] = { v, p };
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(v);
    CFRelease(p);
    return d;
}

static void *hid_thread(void *arg) {
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    /* the bus keeps Sony's vendor id whatever we declare */
    const void *dicts[] = { match_dict(PSP_VID, BRIDGE_PID), match_dict(PSP_VID, PSP_BRIDGE_PID),
                            match_dict(BRIDGE_VID, BRIDGE_PID) };
    CFArrayRef arr = CFArrayCreate(NULL, dicts, 3, &kCFTypeArrayCallBacks);
    IOHIDManagerSetDeviceMatchingMultiple(m, arr);
    IOHIDManagerRegisterDeviceMatchingCallback(m, on_match, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(m, on_remove, NULL);
    IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOReturn r = IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) fprintf(stderr, "pspbridged: IOHIDManagerOpen 0x%x\n", r);
    CFRunLoopRun();
    return NULL;
}

/* Reads what PPSSPP sent back; reports its errors (e.g. unknown button) */
static int ws_drain(int s) {
    char buf[4096];
    for (;;) {
        int n = ws_recv_text(s, buf, sizeof(buf), 0);
        if (n <= 0) return n;
        char *e = strstr(buf, "\"error\"");
        if (e) fprintf(stderr, "pspbridged: PPSSPP says: %.200s\n", e);
    }
}

static int ppsspp_is_running(void) {
    return system("/usr/bin/pgrep -xq PPSSPPSDL") == 0;
}

static void *forward_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    int s = -1;
    uint8_t sent[PAD_REPORT_SIZE];
    int sentPresent = 0;
    unsigned seen = 0;
    char msgs[4096];

    for (;;) {
        if (s < 0) {
            if (!ppsspp_is_running() || (s = ws_connect(port)) < 0) {
                sleep(2);
                continue;
            }
            fprintf(stderr, "pspbridged: connected to PPSSPP's debugger (port %d)\n", port);
            sentPresent = 0; /* resend the whole state */
            seen = 0;
        }
        uint8_t cur[PAD_REPORT_SIZE];
        int present;
        pthread_mutex_lock(&g_padLock);
        if (g_padSeq == seen) {
            struct timeval now;
            gettimeofday(&now, NULL);
            struct timespec until = { now.tv_sec, (long)now.tv_usec * 1000 + 200000000L };
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_padCond, &g_padLock, &until);
        }
        seen = g_padSeq;
        memcpy(cur, g_pad, sizeof(cur));
        present = g_padPresent;
        pthread_mutex_unlock(&g_padLock);

        int n = pad_messages(sentPresent ? sent : NULL, present ? cur : NULL, msgs, sizeof(msgs));
        int failed = 0;
        for (char *m = msgs; n > 0 && *m; m += strlen(m) + 1)
            if (ws_send_text(s, m) != 0) failed = 1;
        if (!failed && ws_drain(s) != 0) failed = 1;
        if (failed) {
            fprintf(stderr, "pspbridged: PPSSPP's debugger gone\n");
            close(s);
            s = -1;
            continue;
        }
        memcpy(sent, cur, sizeof(sent));
        sentPresent = present;
    }
    return NULL;
}

void pad_forward_start(int debuggerPort) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, hid_thread, NULL);
    pthread_detach(t1);
    pthread_create(&t2, NULL, forward_thread, (void *)(intptr_t)debuggerPort);
    pthread_detach(t2);
}
