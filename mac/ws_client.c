/* PSP Bridge — minimal WebSocket client, see ws_client.h */
#include "ws_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int ws_connect(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s);
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* PPSSPP refuses a Sec-WebSocket-Protocol header: none */
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET /debugger HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Key: UFNQIEJyaWRnZSBwYWQhIQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n",
                     port);
    if (send(s, req, (size_t)n, 0) != n) {
        close(s);
        return -1;
    }
    /* read the answer byte by byte: frames may follow right after it */
    char resp[1024];
    int got = 0;
    struct pollfd p = { s, POLLIN, 0 };
    while (got < (int)sizeof(resp) - 1) {
        if (poll(&p, 1, 2000) <= 0) break;
        if (recv(s, resp + got, 1, 0) != 1) break;
        got++;
        resp[got] = 0;
        if (got >= 4 && !memcmp(resp + got - 4, "\r\n\r\n", 4)) break;
    }
    resp[got] = 0;
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        close(s);
        return -1;
    }
    return s;
}

static int send_frame(int s, int opcode, const uint8_t *data, size_t len) {
    uint8_t head[14];
    size_t h = 0;
    head[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        head[h++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        head[h++] = 0x80 | 126;
        head[h++] = (uint8_t)(len >> 8);
        head[h++] = (uint8_t)len;
    } else {
        return -1;
    }
    static const uint8_t mask[4] = { 0x50, 0x53, 0x50, 0x21 };
    memcpy(head + h, mask, 4);
    h += 4;
    uint8_t buf[4096];
    if (h + len > sizeof(buf)) return -1;
    memcpy(buf, head, h);
    for (size_t i = 0; i < len; i++) buf[h + i] = data[i] ^ mask[i & 3];
    return send(s, buf, h + len, 0) == (ssize_t)(h + len) ? 0 : -1;
}

int ws_send_text(int s, const char *text) {
    return send_frame(s, 0x1, (const uint8_t *)text, strlen(text));
}

static int recv_exact(int s, uint8_t *dst, size_t len, int timeoutMs) {
    size_t got = 0;
    struct pollfd p = { s, POLLIN, 0 };
    while (got < len) {
        int r = poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return -1;
        ssize_t n = recv(s, dst + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
        timeoutMs = 2000; /* the rest of a started frame comes quickly */
    }
    return 1;
}

int ws_recv_text(int s, char *buf, int cap, int timeoutMs) {
    for (;;) {
        uint8_t h[2];
        int r = recv_exact(s, h, 2, timeoutMs);
        if (r <= 0) return r;
        int opcode = h[0] & 0x0F;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            uint8_t e[2];
            if (recv_exact(s, e, 2, 2000) <= 0) return -1;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (recv_exact(s, e, 8, 2000) <= 0) return -1;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        }
        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (h[1] & 0x80 && recv_exact(s, mask, 4, 2000) <= 0) return -1;
        /* read the payload, keeping what fits */
        uint64_t kept = 0;
        uint8_t chunk[4096];
        uint64_t left = len;
        while (left) {
            size_t n = left < sizeof(chunk) ? (size_t)left : sizeof(chunk);
            if (recv_exact(s, chunk, n, 2000) <= 0) return -1;
            for (size_t i = 0; i < n; i++) {
                uint8_t c = chunk[i] ^ mask[(len - left + i) & 3];
                if (kept + 1 < (uint64_t)cap) buf[kept++] = (char)c;
            }
            left -= n;
        }
        buf[kept] = 0;
        if (opcode == 0x8) return -1;                       /* close */
        if (opcode == 0x9) {                                /* ping */
            send_frame(s, 0xA, (const uint8_t *)buf, (size_t)kept);
            continue;
        }
        if (opcode == 0x1 || opcode == 0x0) return (int)kept;
        /* binary / pong: skip */
    }
}
