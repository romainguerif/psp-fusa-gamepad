/* Title and game code of a PSP ISO, see iso_info.h */
#include "iso_info.h"

#include <stdlib.h>
#include <string.h>

#define SECTOR 2048

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t len) {
    size_t n = 0;
    while (n < len && n + 1 < cap && src[n]) {
        dst[n] = (char)src[n];
        n++;
    }
    dst[n] = 0;
}

int sfo_parse(const uint8_t *sfo, size_t len, IsoInfo *out) {
    if (len < 20 || memcmp(sfo, "\0PSF", 4) != 0) return -1;
    uint32_t keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
    if (keys > len || data > len || 20 + (uint64_t)count * 16 > len) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = sfo + 20 + i * 16;
        uint32_t k = keys + le16(e), dlen = le32(e + 4), d = data + le32(e + 12);
        if (k >= len || d > len || dlen > len - d) continue;
        const char *key = (const char *)sfo + k;
        size_t keyMax = len - k;
        if (strnlen(key, keyMax) == keyMax) continue;
        if (!strcmp(key, "TITLE")) copy_str(out->title, sizeof(out->title), sfo + d, dlen);
        else if (!strcmp(key, "DISC_ID")) copy_str(out->id, sizeof(out->id), sfo + d, dlen);
    }
    return out->title[0] || out->id[0] ? 0 : -1;
}

/* Looks for `name` in the directory at extent/size; returns 0 and the
   entry's extent/size. `name` is compared without the ";1" version. */
static int find_entry(IsoReadFn read, void *ctx, uint32_t extent, uint32_t size,
                      const char *name, int wantDir, uint32_t *outExtent, uint32_t *outSize) {
    uint8_t sec[SECTOR];
    size_t nameLen = strlen(name);
    if (size > 64 * SECTOR) size = 64 * SECTOR; /* PSP_GAME is small */
    for (uint32_t off = 0; off < size; off += SECTOR) {
        if (read(ctx, (uint64_t)(extent) * SECTOR + off, sec, SECTOR) != SECTOR) return -1;
        uint32_t pos = 0;
        while (pos < SECTOR) {
            uint8_t rlen = sec[pos];
            if (rlen == 0) break; /* rest of the sector is padding */
            if (rlen < 34 || pos + rlen > SECTOR) return -1;
            const uint8_t *r = sec + pos;
            uint8_t nlen = r[32];
            if (33 + nlen > rlen) return -1;
            const char *n = (const char *)r + 33;
            size_t cmpLen = nlen;
            for (size_t i = 0; i < nlen; i++)
                if (n[i] == ';') { cmpLen = i; break; }
            if (cmpLen == nameLen && !strncmp(n, name, nameLen) &&
                ((r[25] & 2) != 0) == (wantDir != 0)) {
                *outExtent = le32(r + 2);
                *outSize = le32(r + 10);
                return 0;
            }
            pos += rlen;
        }
    }
    return -1;
}

int iso_info_read(IsoReadFn read, void *ctx, IsoInfo *out) {
    uint8_t pvd[SECTOR];
    uint32_t ext, size, sfoExt, sfoSize;
    memset(out, 0, sizeof(*out));
    if (read(ctx, 16 * SECTOR, pvd, SECTOR) != SECTOR) return -1;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    const uint8_t *root = pvd + 156;
    if (find_entry(read, ctx, le32(root + 2), le32(root + 10), "PSP_GAME", 1, &ext, &size)) return -1;
    if (find_entry(read, ctx, ext, size, "PARAM.SFO", 0, &sfoExt, &sfoSize)) return -1;
    if (sfoSize == 0 || sfoSize > 64 * 1024) return -1;
    uint8_t *sfo = malloc(sfoSize);
    if (!sfo) return -1;
    int r = -1;
    if (read(ctx, (uint64_t)sfoExt * SECTOR, sfo, sfoSize) == (int)sfoSize)
        r = sfo_parse(sfo, sfoSize, out);
    free(sfo);
    return r;
}
