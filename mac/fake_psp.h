/*
 * In-process fake PSP for the tests: runs the PSP side of the protocol
 * (bridge_proto.c, the very code of the driver) behind a BridgeTransport,
 * with the same exact-size transfer rules as the driver's loop.
 */
#ifndef FAKE_PSP_H
#define FAKE_PSP_H

#include "bridge_client.h"

typedef struct FakePsp FakePsp;

FakePsp *fake_psp_new(void);
void fake_psp_free(FakePsp *f);
BridgeTransport fake_psp_transport(FakePsp *f);
unsigned fake_psp_requests(const FakePsp *f);

#endif
