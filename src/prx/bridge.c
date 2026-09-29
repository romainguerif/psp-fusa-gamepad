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

static int g_logLines = 0;

static void str_append(char *dst, const char *src, int max)
{
	int n = strlen(dst);
	while (*src && n < max - 1)
		dst[n++] = *src++;
	dst[n] = 0;
}

void bridge_log(const char *msg, unsigned int value)
{
	static const char hex[] = "0123456789ABCDEF";
	char line[96];
	char num[12];
	int i;

	if (g_logLines >= 200)
		return;
	line[0] = 0;
	str_append(line, msg, sizeof(line));
	num[0] = ' '; num[1] = '0'; num[2] = 'x';
	for (i = 0; i < 8; i++)
		num[3 + i] = hex[(value >> (28 - 4 * i)) & 0xF];
	num[11] = 0;
	str_append(line, num, sizeof(line));
	str_append(line, "\r\n", sizeof(line));

	SceUID fd = sceIoOpen("ms0:/pspbridge.log", PSP_O_WRONLY | PSP_O_CREAT |
	                      (g_logLines == 0 ? PSP_O_TRUNC : PSP_O_APPEND), 0777);
	if (fd >= 0) {
		sceIoWrite(fd, line, strlen(line));
		sceIoClose(fd);
	}
	g_logLines++;
}

/* --- transfers ------------------------------------------------------------ */

static void outDone(struct UsbbdDeviceRequest *req)
{
	sceKernelSetEventFlag(g_evt, EVT_OUT_DONE);
}

static void inDone(struct UsbbdDeviceRequest *req)
{
	sceKernelSetEventFlag(g_evt, EVT_IN_DONE);
}

static int connected(void)
{
	return (sceUsbGetState() & PSP_USB_STATUS_CONNECTION_ESTABLISHED) != 0;
}

/* Receives (out=1) or sends (out=0) exactly `size` bytes. Returns the number
   of bytes transferred, or < 0 (error, cancelled, cable removed, stop). */
static int transfer(int out, void *data, int size)
{
	struct UsbbdDeviceRequest *req = out ? &g_outReq : &g_inReq;
	unsigned int bit = out ? EVT_OUT_DONE : EVT_IN_DONE;
	int lines = (size + 63) & ~63;
	int ret;

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

	ret = out ? sceUsbbdReqRecv(req) : sceUsbbdReqSend(req);
	if (ret < 0) {
		bridge_log(out ? "ReqRecv failed" : "ReqSend failed", ret);
		return ret;
	}

	for (;;) {
		SceUInt timeout = WAIT_SLICE_US;
		u32 result;
		result = 0;
		if (sceKernelWaitEventFlag(g_evt, bit | EVT_RESET, PSP_EVENT_WAITOR,
		                           &result, &timeout) == 0 && (result & bit)) {
			sceKernelClearEventFlag(g_evt, ~bit);
			break;
		}
		if ((result & EVT_RESET) || !g_running || !connected()) {
			if (result & EVT_RESET)
				bridge_log("reset by host", 0);
			sceUsbbdReqCancelAll(req->endpoint);
			timeout = WAIT_SLICE_US;
			sceKernelWaitEventFlag(g_evt, bit, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR,
			                       &result, &timeout);
			sceKernelClearEventFlag(g_evt, ~EVT_RESET);
			return -1;
		}
	}
	if (req->returnCode != 0)
		return -1;
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
			if (wasReady)
				bridge_log("host gone, requests", g_requests);
			wasReady = 0;
			g_state = BRIDGE_STATE_WAITING;
			sceKernelDelayThread(50 * 1000);
			continue;
		}
		if (!wasReady)
			bridge_log("host connected", sceUsbGetState());
		wasReady = 1;
		g_state = BRIDGE_STATE_READY;

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
			bridge_handle(&req, g_payload, g_requests, &resp);
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
	/* gamepad thread is 32: lower priority = higher number */
	g_thid = sceKernelCreateThread("bridge_thread", bridge_thread, 40, 0x1000, 0, NULL);
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
	if (g_evt >= 0) {
		sceKernelDeleteEventFlag(g_evt);
		g_evt = -1;
	}
	g_state = BRIDGE_STATE_OFF;
}

void bridge_reset(void)
{
	if (g_evt >= 0)
		sceKernelSetEventFlag(g_evt, EVT_RESET);
}

int bridge_status(int *requests)
{
	if (requests)
		*requests = (int)g_requests;
	return g_state;
}
