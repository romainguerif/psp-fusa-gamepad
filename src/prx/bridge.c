/*
 * PSP Bridge — file channel on USB interface 1.
 *
 * One kernel thread, below the gamepad thread in priority: the HID reports
 * on endpoint 1 never wait for the file channel. It serves one request at a
 * time: read the 16-byte header, read the payload, answer (header, then
 * payload). Protocol and command handling: ../common/bridge_proto.c.
 */
#include <pspkernel.h>
#include <string.h>
#include "bridge.h"
#include "../common/bridge_proto.h"
#include "psp_fs.h"

#define EVT_OUT_DONE 0x1
#define EVT_IN_DONE  0x2
#define EVT_RESET    0x4

/* Wakes up regularly to notice a stop or a cable removal */
#define WAIT_SLICE_US (100 * 1000)

static struct UsbEndpoint *g_epOut, *g_epIn;
static SceUID g_evt = -1;
static SceUID g_thid = -1;
static volatile int g_running = 0;
static volatile int g_state = BRIDGE_STATE_OFF;
static volatile unsigned int g_requests = 0;

static struct UsbbdDeviceRequest g_outReq, g_inReq;
/* DMA buffers: 64-byte aligned, whole cache lines */
static unsigned char g_hdr[64] __attribute__((aligned(64)));
static unsigned char g_payload[BRIDGE_MAX_PAYLOAD] __attribute__((aligned(64)));

/* --- log ------------------------------------------------------------------ */
/*
 * Lines go to a RAM ring (safe from the USB callbacks, never blocks) and are
 * written to ms0:/pspbridge.log by bridge_flush_log(), called from the
 * loader's thread: if the bridge thread hangs, its last lines still reach
 * the file. `msg` must be a string constant.
 */
#define TRACE 512
static const char *g_trMsg[TRACE];
static unsigned int g_trVal[TRACE], g_trTime[TRACE];
static volatile unsigned int g_trHead = 0, g_trTail = 0, g_trLost = 0;
static int g_fileLines = 0;
/* step-by-step tracing of the transfers: the next lines after a reset */
static volatile int g_trace = 0;
#define TRACE_LINES_AFTER_RESET 300
#define TR(msg, v) do { if (g_trace > 0) { g_trace--; bridge_log(msg, v); } } while (0)

void bridge_log(const char *msg, unsigned int value)
{
	int intr = sceKernelCpuSuspendIntr();
	unsigned int h = g_trHead;
	if (h - g_trTail < TRACE) {
		g_trMsg[h % TRACE] = msg;
		g_trVal[h % TRACE] = value;
		g_trTime[h % TRACE] = sceKernelGetSystemTimeLow();
		g_trHead = h + 1;
	} else {
		g_trLost++;
	}
	sceKernelCpuResumeIntr(intr);
}

void bridge_note(const char *msg, unsigned int value)
{
	bridge_log(msg, value);
}

static void put_hex(char *p, unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";
	int i;
	for (i = 0; i < 8; i++)
		p[i] = hex[(v >> (28 - 4 * i)) & 0xF];
}

static void put_dec(char *p, unsigned int v, int width)
{
	int i;
	for (i = width - 1; i >= 0; i--) {
		p[i] = '0' + v % 10;
		v /= 10;
	}
}

void bridge_flush_log(void)
{
	static char buf[4096];
	int len = 0;
	SceUID fd;

	if (g_trTail == g_trHead || g_fileLines >= 5000)
		return;
	fd = sceIoOpen("ms0:/pspbridge.log", PSP_O_WRONLY | PSP_O_CREAT |
	               (g_fileLines == 0 ? PSP_O_TRUNC : PSP_O_APPEND), 0777);
	while (g_trTail != g_trHead) {
		unsigned int t = g_trTail;
		const char *msg = g_trMsg[t % TRACE];
		int n = strlen(msg);
		if (n > 60) n = 60;
		if (len + 90 > (int)sizeof(buf)) {
			if (fd >= 0) sceIoWrite(fd, buf, len);
			len = 0;
		}
		/* "[  12345.678] message 0x0000ABCD" */
		buf[len++] = '[';
		put_dec(buf + len, g_trTime[t % TRACE] / 1000, 8);
		len += 8;
		buf[len++] = '.';
		put_dec(buf + len, g_trTime[t % TRACE] % 1000, 3);
		len += 3;
		buf[len++] = ']';
		buf[len++] = ' ';
		memcpy(buf + len, msg, n);
		len += n;
		buf[len++] = ' ';
		buf[len++] = '0';
		buf[len++] = 'x';
		put_hex(buf + len, g_trVal[t % TRACE]);
		len += 8;
		buf[len++] = '\r';
		buf[len++] = '\n';
		g_trTail = t + 1;
		g_fileLines++;
	}
	if (g_trLost) {
		const char *lost = "(lines lost: ring full)\r\n";
		memcpy(buf + len, lost, strlen(lost));
		len += strlen(lost);
		g_trLost = 0;
	}
	if (fd >= 0) {
		sceIoWrite(fd, buf, len);
		sceIoClose(fd);
	}
}

/* --- watchdog -------------------------------------------------------------- */
/* Where the bridge thread is, and a counter it bumps whenever it runs */
#define W_IDLE      1  /* no host, sleeping */
#define W_RELEASE   2  /* waiting for the bus to give a request back */
#define W_RECV      3  /* inside sceUsbbdReqRecv */
#define W_SEND      4  /* inside sceUsbbdReqSend */
#define W_WAIT      5  /* waiting for a completion (100 ms slices) */
#define W_CANCEL    6  /* inside sceUsbbdReqCancelAll */
#define W_HANDLE    7  /* running a command */
static volatile int g_where = 0;
static volatile unsigned int g_alive = 0;

/* --- transfers ------------------------------------------------------------ */

/* A request belongs to the bus from ReqRecv/ReqSend until its completion
   callback has run, also after a cancel (the bus completes cancelled
   requests later, in the same structure). busy = still owned by the bus. */
static volatile int g_outBusy = 0, g_inBusy = 0;

static void outDone(struct UsbbdDeviceRequest *req)
{
	g_outBusy = 0;
	sceKernelSetEventFlag(g_evt, EVT_OUT_DONE);
}

static void inDone(struct UsbbdDeviceRequest *req)
{
	g_inBusy = 0;
	sceKernelSetEventFlag(g_evt, EVT_IN_DONE);
}

static int connected(void)
{
	return (sceUsbGetState() & PSP_USB_STATUS_CONNECTION_ESTABLISHED) != 0;
}

/* Waits (at most ~1 s) until the bus has given the request back */
static int wait_released(volatile int *busy)
{
	int i;
	g_where = W_RELEASE;
	for (i = 0; i < 1000 && *busy; i++) {
		g_alive++;
		sceKernelDelayThread(1000);
	}
	return !*busy;
}

/* Receives (out=1) or sends (out=0) exactly `size` bytes. Returns the number
   of bytes transferred, or < 0 (error, cancelled, cable removed, stop). */
static int transfer(int out, void *data, int size)
{
	struct UsbbdDeviceRequest *req = out ? &g_outReq : &g_inReq;
	volatile int *busy = out ? &g_outBusy : &g_inBusy;
	unsigned int bit = out ? EVT_OUT_DONE : EVT_IN_DONE;
	int lines = (size + 63) & ~63;
	int ret;

	/* never touch a request the bus still holds */
	if (!wait_released(busy)) {
		bridge_log(out ? "OUT request still held by the bus" : "IN request still held by the bus", 0);
		return -1;
	}

	if (out)
		sceKernelDcacheInvalidateRange(data, lines);
	else
		sceKernelDcacheWritebackRange(data, lines);

	memset(req, 0, sizeof(*req));
	req->endpoint = out ? g_epOut : g_epIn;
	req->data = data;
	req->size = size;
	req->onComplete = out ? outDone : inDone;
	sceKernelClearEventFlag(g_evt, ~bit);

	*busy = 1;
	TR(out ? "t recv queue, size" : "t send queue, size", size);
	g_where = out ? W_RECV : W_SEND;
	ret = out ? sceUsbbdReqRecv(req) : sceUsbbdReqSend(req);
	g_alive++;
	TR("t   queued, ret", ret);
	if (ret < 0) {
		*busy = 0;
		bridge_log(out ? "ReqRecv failed" : "ReqSend failed", ret);
		return ret;
	}

	for (;;) {
		SceUInt timeout = WAIT_SLICE_US;
		u32 result = 0;
		g_where = W_WAIT;
		g_alive++;
		sceKernelWaitEventFlag(g_evt, bit | EVT_RESET, PSP_EVENT_WAITOR, &result, &timeout);
		if (result)
			TR("t   woken, flags/busy", (result << 8) | *busy);
		if (!*busy) {
			/* completed (the flag bit alone can be a stale one) */
			sceKernelClearEventFlag(g_evt, ~bit);
			break;
		}
		if ((result & EVT_RESET) || !g_running || !connected()) {
			bridge_log((result & EVT_RESET) ? "reset by host, cancel" : "cancel, stop or unplugged", out);
			g_where = W_CANCEL;
			ret = sceUsbbdReqCancelAll(req->endpoint);
			g_alive++;
			TR("t   cancel ret", ret);
			if (!wait_released(busy))
				bridge_log("cancel not completed by the bus", out);
			sceKernelClearEventFlag(g_evt, ~(bit | EVT_RESET));
			return -1;
		}
	}
	TR(out ? "t   recv done, bytes" : "t   send done, bytes", req->transmitted);
	if (req->returnCode != 0) {
		bridge_log(out ? "recv returnCode" : "send returnCode", req->returnCode);
		return -1;
	}
	if (out)
		sceKernelDcacheInvalidateRange(data, lines);
	return req->transmitted;
}

/* --- request loop --------------------------------------------------------- */

static int bridge_thread(SceSize args, void *argp)
{
	int wasReady = 0;

	while (g_running) {
		BridgeHeader req, resp;
		int n, err;

		if (!connected()) {
			if (wasReady) {
				bridge_log("host gone, requests", g_requests);
				psp_fs_close();
			}
			wasReady = 0;
			g_state = BRIDGE_STATE_WAITING;
			g_where = W_IDLE;
			g_alive++;
			sceKernelDelayThread(50 * 1000);
			continue;
		}
		if (!wasReady) {
			bridge_log("host connected", sceUsbGetState());
			bridge_log("bus ep numbers out/in", (g_epOut->endpointNumber << 16) | g_epIn->endpointNumber);
		}
		wasReady = 1;
		g_state = BRIDGE_STATE_READY;

		TR("t loop: wait header", g_requests);
		n = transfer(1, g_hdr, BRIDGE_HEADER_SIZE);
		if (n < 0)
			continue;
		memset(&req, 0, sizeof(req));
		if (n == BRIDGE_HEADER_SIZE) {
			bridge_get_header(g_hdr, &req);
			err = bridge_check_request(&req);
		} else {
			err = BRIDGE_ERR_MAGIC;
		}

		if (err == 0 && req.len > 0) {
			n = transfer(1, g_payload, req.len);
			if (n != (int)req.len) {
				bridge_log("short payload", n);
				continue;
			}
		}
		if (err == 0) {
			g_requests++;
			if (g_requests == 1)
				bridge_log("first request, cmd", req.cmd);
			g_where = W_HANDLE;
			bridge_handle(&req, g_payload, g_requests, psp_fs(), &resp);
		} else {
			bridge_log("bad request", (unsigned int)err);
			bridge_error_response(&req, err, &resp);
		}

		bridge_put_header(g_hdr, &resp);
		if (transfer(0, g_hdr, BRIDGE_HEADER_SIZE) != BRIDGE_HEADER_SIZE)
			continue;
		if (resp.len > 0)
			transfer(0, g_payload, resp.len);
	}
	g_state = BRIDGE_STATE_OFF;
	return 0;
}

int bridge_start(struct UsbEndpoint *out, struct UsbEndpoint *in)
{
	g_epOut = out;
	g_epIn = in;
	g_requests = 0;

	g_evt = sceKernelCreateEventFlag("BridgeEvent", 0, 0, NULL);
	if (g_evt < 0) {
		bridge_log("event flag failed", g_evt);
		g_state = BRIDGE_STATE_ERROR;
		return g_evt;
	}
	g_running = 1;
	/* 31, just above the gamepad thread (32). At 40 it was starved: when a
	   client dies mid-transfer, a system thread between the two spins for
	   seconds (seen on a PSP-3000, 29/09). This thread is IO-bound: it
	   sleeps in its waits and never holds the CPU for long. */
	g_thid = sceKernelCreateThread("bridge_thread", bridge_thread, 31, 0x2000, 0, NULL);
	if (g_thid < 0 || sceKernelStartThread(g_thid, 0, NULL) < 0) {
		bridge_log("thread failed", g_thid);
		g_running = 0;
		g_state = BRIDGE_STATE_ERROR;
		return -1;
	}
	g_state = BRIDGE_STATE_WAITING;
	bridge_log("file channel started, max payload", BRIDGE_MAX_PAYLOAD);
	return 0;
}

void bridge_stop(void)
{
	if (g_thid >= 0) {
		SceUInt timeout = 1000 * 1000;
		g_running = 0;
		if (sceKernelWaitThreadEnd(g_thid, &timeout) < 0)
			sceKernelTerminateThread(g_thid);
		sceKernelDeleteThread(g_thid);
		g_thid = -1;
	}
	psp_fs_close();
	if (g_evt >= 0) {
		sceKernelDeleteEventFlag(g_evt);
		g_evt = -1;
	}
	g_state = BRIDGE_STATE_OFF;
	bridge_flush_log();
}

/* Called every ~0.5 s from the loader's thread. If the bridge thread has
   not run for 2 s, logs where it is, then raises its priority once: if it
   comes back, it was starved (not blocked). */
void bridge_watchdog(void)
{
	static unsigned int last = 0;
	static int still = 0, boosted = 0, reported = 0;
	if (g_thid < 0)
		return;
	if (g_alive != last) {
		if (reported) {
			bridge_log("watchdog: thread runs again, where", g_where);
			reported = 0;
		}
		last = g_alive;
		still = 0;
		return;
	}
	if (++still == 4) {
		bridge_log("watchdog: no progress for 2 s, where", g_where);
		reported = 1;
		if (!boosted) {
			boosted = 1;
			bridge_log("watchdog: priority -> 17, ret",
			           sceKernelChangeThreadPriority(g_thid, 17));
		}
	}
}

void bridge_reset(void)
{
	g_trace = TRACE_LINES_AFTER_RESET;
	if (g_evt >= 0)
		sceKernelSetEventFlag(g_evt, EVT_RESET);
}

int bridge_status(int *requests)
{
	if (requests)
		*requests = (int)g_requests;
	return g_state;
}
