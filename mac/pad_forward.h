/*
 * PSP Bridge — the PSP's gamepad into PPSSPP, bypassing SDL.
 *
 * Since macOS 26.6, Apple's GameController framework claims every HID
 * device with Sony's vendor id (the PSP's bus forces it) without driving
 * it, and the SDL inside PPSSPP then ignores the pad. We read the pad's HID
 * reports ourselves and send buttons and analog stick to PPSSPP's remote
 * debugger (WebSocket ws://127.0.0.1:<port>/debugger, events
 * input.buttons.send / input.analog.send). This drives the emulated PSP:
 * games, not PPSSPP's own menus.
 */
#ifndef PAD_FORWARD_H
#define PAD_FORWARD_H

#include <stdint.h>

#define PPSSPP_DEBUGGER_PORT 8766

/* Starts the HID reader and the forwarding thread (runs forever) */
void pad_forward_start(int debuggerPort);

/* Pure helpers, exposed for the tests */
#define PAD_REPORT_SIZE 8
/* JSON messages for the changes from `prev` to `cur` (either may be NULL
   for "everything released"), written to out (NUL separated, ends with an
   empty string). Returns the number of messages. */
int pad_messages(const uint8_t *prev, const uint8_t *cur, char *out, int outSize);
/* Stick byte (0..255, 128 = centre) -> -1..1 with a dead zone; y is
   inverted (PSP: 0 = up, PPSSPP: +1 = up) by the caller */
float pad_axis(int raw);

#endif
