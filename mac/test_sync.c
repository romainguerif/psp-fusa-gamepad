/* Save synchronisation against the fake PSP (a folder playing ms0:/) and a
   folder playing PPSSPP's SAVEDATA. */
#include "save_sync.h"
#include "fake_psp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static BridgeClient g_c;
static int t_list(void *x, const char *d, BridgeListFn fn, void *u) { return bridge_list(&g_c, d, fn, u); }
static int t_read(void *x, const char *p, uint8_t **d, uint32_t *l) { return bridge_read_file(&g_c, p, d, l); }
static int t_write(void *x, const char *p, const uint8_t *d, uint32_t l) { return bridge_write_file(&g_c, p, d, l); }
static int t_mkdir(void *x, const char *p) { return bridge_mkdir(&g_c, p); }
static int t_rename(void *x, const char *a, const char *b) { return bridge_rename(&g_c, a, b); }
static int t_remove(void *x, const char *p) { return bridge_remove(&g_c, p); }

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static int same(const char *path, const char *text) {
    char buf[256] = "";
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int count_entries(const char *dir) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *de;
    if (!d) return 0;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

int main(void) {
    char root[] = "/tmp/pspbridge-sync-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char psp[600], mac[600], bak[600], p[900];
    snprintf(psp, sizeof(psp), "%s/ms0/PSP/SAVEDATA", root);
    snprintf(mac, sizeof(mac), "%s/ppsspp/PSP/SAVEDATA", root);
    snprintf(bak, sizeof(bak), "%s/backups", root);
    snprintf(p, sizeof(p), "mkdir -p '%s' '%s'", psp, mac);
    CHECK(system(p) == 0);

    /* PSP: the game's save + another game's; PPSSPP: an older copy of the
       same save + a save the PSP does not have yet */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00", psp); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", psp); put(p, "psp-progress-7");
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/PARAM.SFO", psp); put(p, "sfo-psp");
    snprintf(p, sizeof(p), "%s/ULES00999DATA", psp); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ULES00999DATA/DATA.BIN", psp); put(p, "other game");
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00", mac); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", mac); put(p, "ppsspp-progress-3");
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/OLD.BIN", mac); put(p, "gone on the psp");
    snprintf(p, sizeof(p), "%s/ULUS10041SYS", mac); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ULUS10041SYS/SYS.BIN", mac); put(p, "settings");

    char ms0[600];
    snprintf(ms0, sizeof(ms0), "%s/ms0", root);
    FakePsp *f = fake_psp_new(ms0);
    bridge_client_init(&g_c, fake_psp_transport(f));
    SyncConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.macSaveDir, sizeof(cfg.macSaveDir), "%s", mac);
    snprintf(cfg.backupDir, sizeof(cfg.backupDir), "%s", bak);
    SyncPsp sp = { t_list, t_read, t_write, t_mkdir, t_rename, t_remove, NULL };
    cfg.psp = sp;
    cfg.keepBackups = 3;

    /* --- game start: the PSP wins -------------------------------------------- */
    CHECK(sync_game_start(&cfg, "ULUS10041") == 2);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", mac);
    CHECK(same(p, "psp-progress-7"));
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/PARAM.SFO", mac);
    CHECK(same(p, "sfo-psp"));
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/OLD.BIN", mac);
    CHECK(!exists(p));                                   /* exactly the PSP's folder */
    /* PPSSPP's version was kept */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00", bak);
    CHECK(count_entries(p) == 1);
    /* PPSSPP-only folder went to the PSP */
    snprintf(p, sizeof(p), "%s/ULUS10041SYS/SYS.BIN", psp);
    CHECK(same(p, "settings"));
    /* the other game is untouched and not copied */
    snprintf(p, sizeof(p), "%s/ULES00999DATA", mac);
    CHECK(!exists(p));
    /* a second start changes nothing */
    CHECK(sync_game_start(&cfg, "ULUS10041") == 0);

    /* --- during the game: PPSSPP saves ------------------------------------------ */
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 0);    /* nothing changed */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", mac);
    put(p, "ppsspp-progress-8");
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 0);    /* changed, not yet stable */
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 1);    /* stable: pushed */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", psp);
    CHECK(same(p, "ppsspp-progress-8"));
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN.pbtmp", psp);
    CHECK(!exists(p));                                   /* no temp left */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00", bak);
    CHECK(count_entries(p) == 2);                        /* + the PSP's previous version */
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 0);    /* done */

    /* a new save folder created during the game */
    snprintf(p, sizeof(p), "%s/ULUS10041DATA01", mac); mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA01/DATA.BIN", mac); put(p, "slot 2");
    sync_push_changes(&cfg, "ULUS10041");
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 1);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA01/DATA.BIN", psp);
    CHECK(same(p, "slot 2"));

    /* PSP unreachable: the push fails, then succeeds once it is back */
    fake_psp_free(f);
    f = fake_psp_new(NULL);                              /* Memory Stick errors */
    bridge_client_init(&g_c, fake_psp_transport(f));
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", mac);
    put(p, "ppsspp-progress-9");
    sync_push_changes(&cfg, "ULUS10041");
    CHECK(sync_push_changes(&cfg, "ULUS10041") < 0);
    fake_psp_free(f);
    f = fake_psp_new(ms0);
    bridge_client_init(&g_c, fake_psp_transport(f));
    CHECK(sync_push_changes(&cfg, "ULUS10041") == 1);
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", psp);
    CHECK(same(p, "ppsspp-progress-9"));

    /* backups are capped (keepBackups = 3 here) */
    for (int k = 0; k < 5; k++) {
        char text[32];
        snprintf(text, sizeof(text), "round %d", k);
        snprintf(p, sizeof(p), "%s/ULUS10041DATA00/DATA.BIN", mac);
        put(p, text);
        sync_push_changes(&cfg, "ULUS10041");
        sync_push_changes(&cfg, "ULUS10041");
    }
    snprintf(p, sizeof(p), "%s/ULUS10041DATA00", bak);
    CHECK(count_entries(p) == 3);
    /* the 3 kept are the newest: rounds 1, 2 and 3 were the PSP's versions
       replaced by rounds 2, 3 and 4 */
    {
        DIR *d = opendir(p);
        struct dirent *de;
        int newest = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char q[1200];
            snprintf(q, sizeof(q), "%s/%s/DATA.BIN", p, de->d_name);
            if (same(q, "round 1") || same(q, "round 2") || same(q, "round 3")) newest++;
        }
        closedir(d);
        CHECK(newest == 3);
    }

    fake_psp_free(f);
    snprintf(p, sizeof(p), "rm -rf '%s'", root);
    CHECK(system(p) == 0);
    printf(failures ? "%d failure(s)\n" : "SYNC OK\n", failures);
    return failures ? 1 : 0;
}
