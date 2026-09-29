/*
 * In-process fake PSP for the tests: runs the PSP side of the protocol
 * (bridge_proto.c, the very code of the driver) behind a BridgeTransport,
 * with the same exact-size transfer rules as the driver's loop.
 */
#ifndef FAKE_PSP_H
#define FAKE_PSP_H

#include "bridge_client.h"

typedef struct FakePsp FakePsp;

/* `root` is the folder that plays the Memory Stick (ms0:/), or NULL: then
   STAT/LIST/READ answer BRIDGE_ERR_IO */
FakePsp *fake_psp_new(const char *root);
void fake_psp_free(FakePsp *f);
BridgeTransport fake_psp_transport(FakePsp *f);
unsigned fake_psp_requests(const FakePsp *f);

#endif
