/* PSP Bridge — minimal WebSocket client for PPSSPP's local debugger
   (ws://127.0.0.1:<port>/debugger). Text frames only; never leaves the Mac. */
#ifndef WS_CLIENT_H
#define WS_CLIENT_H

/* Connected socket, or -1 */
int ws_connect(int port);
int ws_send_text(int s, const char *text);
/* Next text message into buf (NUL terminated). Returns its length, 0 on
   timeout, -1 if the connection is gone. Answers pings. */
int ws_recv_text(int s, char *buf, int cap, int timeoutMs);

#endif
