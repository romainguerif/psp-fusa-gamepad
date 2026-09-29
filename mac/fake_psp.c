/* In-process fake PSP, see fake_psp.h. Mirrors bridge_thread() of
   ../src/prx/bridge.c: header transfer, payload transfer, answer. */
#include "fake_psp.h"

#include <stdlib.h>
#include <string.h>

struct FakePsp {
    /* what the PSP expects next from the host */
    int wantPayload;
    BridgeHeader req;
    uint8_t payload[BRIDGE_MAX_PAYLOAD];
    unsigned requests;
    /* what the PSP has queued for the host: whole transfers */
    uint8_t out[BRIDGE_HEADER_SIZE + BRIDGE_MAX_PAYLOAD];
    int outHdr;     /* bytes of header waiting (0 or 16) */
    int outPayload; /* bytes of payload waiting */
};

FakePsp *fake_psp_new(void) {
    return calloc(1, sizeof(FakePsp));
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
    bridge_handle(&f->req, f->payload, f->requests, &resp);
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
