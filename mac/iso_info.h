/*
 * Title and game code of a PSP ISO: ISO9660 -> PSP_GAME/PARAM.SFO -> TITLE,
 * DISC_ID. The ISO is read through a callback (over USB from the PSP).
 */
#ifndef ISO_INFO_H
#define ISO_INFO_H

#include <stddef.h>
#include <stdint.h>

/* Reads len bytes at offset into dst; returns bytes read or < 0 */
typedef int (*IsoReadFn)(void *ctx, uint64_t offset, uint8_t *dst, uint32_t len);

typedef struct {
    char title[128]; /* UTF-8 */
    char id[16];     /* e.g. ULUS10041 */
} IsoInfo;

/* 0 on success, -1 if this is not a PSP ISO or a read failed */
int iso_info_read(IsoReadFn read, void *ctx, IsoInfo *out);

/* PARAM.SFO parser (exposed for the tests). 0 on success. */
int sfo_parse(const uint8_t *sfo, size_t len, IsoInfo *out);

#endif
