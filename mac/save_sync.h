/*
 * PSP Bridge — step 3: game saves live on the PSP, PPSSPP works on a copy.
 *
 * A game's save folders are the ones under PSP/SAVEDATA whose name starts
 * with its id (e.g. ULUS10041DATA00). Rules (decided with Romain, 29/09):
 *   - game start: the PSP wins. Any folder that differs is copied from the
 *     PSP to PPSSPP, after PPSSPP's version went to the backups. A folder
 *     only PPSSPP has is copied to the PSP.
 *   - during the game: a folder PPSSPP changed, once it stopped changing, is
 *     copied to the PSP (the PSP's previous version goes to the backups
 *     first), each file through a temporary name, then read back and
 *     compared.
 * Backups: <backupDir>/<folder>/<date>-<psp|ppsspp>/, 20 kept per folder.
 * Nothing is ever overwritten or deleted without a backup.
 */
#ifndef SAVE_SYNC_H
#define SAVE_SYNC_H

#include "bridge_client.h"

/* The PSP side, through the file channel (each returns BRIDGE_OK or error) */
typedef struct {
    int (*list)(void *ctx, const char *dir, BridgeListFn fn, void *user);
    int (*read_file)(void *ctx, const char *path, uint8_t **data, uint32_t *len);
    int (*write_file)(void *ctx, const char *path, const uint8_t *data, uint32_t len);
    int (*mkdir)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*remove)(void *ctx, const char *path);
    void *ctx;
} SyncPsp;

typedef struct {
    char macSaveDir[1024];  /* PPSSPP's .../PSP/SAVEDATA */
    char backupDir[1024];
    SyncPsp psp;
    int keepBackups;        /* 20 */
} SyncConfig;

/* Game start: PSP -> PPSSPP (PSP wins), PPSSPP-only folders -> PSP.
   Returns the number of folders copied, or < 0 on a PSP error. */
int sync_game_start(SyncConfig *cfg, const char *gameId);

/* During the game (call every ~2 s): pushes the folders PPSSPP changed and
   that did not change since the previous call. Returns the number pushed,
   or < 0 on error (retried at the next call). */
int sync_push_changes(SyncConfig *cfg, const char *gameId);

/* Forget what was last synced (new game / reconnection) */
void sync_reset(void);

#endif
