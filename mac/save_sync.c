/* PSP Bridge — save synchronisation, see save_sync.h */
#include "save_sync.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_FILES 64
#define MAX_FOLDERS 64
#define PSP_SAVEDATA "ms0:/PSP/SAVEDATA"

/* --- a save folder in memory ------------------------------------------------ */

typedef struct {
    int n;
    char name[MAX_FILES][128];
    uint8_t *data[MAX_FILES];
    uint32_t len[MAX_FILES];
} Folder;

static void folder_free(Folder *f) {
    for (int i = 0; i < f->n; i++) free(f->data[i]);
    f->n = 0;
}

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* files sorted by name so that two copies compare and hash the same */
static void folder_sort(Folder *f) {
    for (int i = 1; i < f->n; i++)
        for (int j = i; j > 0 && cmp_names(f->name[j - 1], f->name[j]) > 0; j--) {
            char t[128];
            memcpy(t, f->name[j], 128); memcpy(f->name[j], f->name[j - 1], 128); memcpy(f->name[j - 1], t, 128);
            uint8_t *d = f->data[j]; f->data[j] = f->data[j - 1]; f->data[j - 1] = d;
            uint32_t l = f->len[j]; f->len[j] = f->len[j - 1]; f->len[j - 1] = l;
        }
}

static uint64_t folder_hash(const Folder *f) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < f->n; i++) {
        for (const char *c = f->name[i]; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
        h = (h ^ 0xFF) * 1099511628211ull;
        for (uint32_t k = 0; k < f->len[i]; k++) h = (h ^ f->data[i][k]) * 1099511628211ull;
        h = (h ^ f->len[i]) * 1099511628211ull;
    }
    return h ^ (uint64_t)f->n;
}

/* our temporary files on either side are not part of a save */
static int is_temp(const char *name) {
    size_t n = strlen(name);
    return name[0] == '.' || (n > 7 && !strcmp(name + n - 7, ".pbtmp"));
}

/* --- Mac side --------------------------------------------------------------- */

static int mac_read(const char *dir, Folder *f) {
    memset(f, 0, sizeof(*f));
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) && f->n < MAX_FILES) {
        char p[2048];
        struct stat st;
        if (is_temp(de->d_name) || strlen(de->d_name) >= 128) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        FILE *fp = fopen(p, "rb");
        if (!fp) continue;
        uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
        size_t got = fread(buf, 1, (size_t)st.st_size, fp);
        fclose(fp);
        snprintf(f->name[f->n], 128, "%s", de->d_name);
        f->data[f->n] = buf;
        f->len[f->n] = (uint32_t)got;
        f->n++;
    }
    closedir(d);
    folder_sort(f);
    return 1;
}

static int mkdirs(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int write_all(const char *path, const uint8_t *data, uint32_t len) {
    char tmp[2100];
    snprintf(tmp, sizeof(tmp), "%s.pbtmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    size_t w = len ? fwrite(data, 1, len, fp) : 0;
    int ok = (w == len) && fflush(fp) == 0;
    fclose(fp);
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Makes the Mac folder exactly `f` (extra files removed) */
static int mac_write(const char *dir, const Folder *f) {
    if (mkdirs(dir)) return -1;
    for (int i = 0; i < f->n; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", dir, f->name[i]);
        if (write_all(p, f->data[i], f->len[i])) return -1;
    }
    Folder cur;
    mac_read(dir, &cur);
    for (int i = 0; i < cur.n; i++) {
        int keep = 0;
        for (int j = 0; j < f->n && !keep; j++) keep = !strcmp(cur.name[i], f->name[j]);
        if (!keep) {
            char p[2048];
            snprintf(p, sizeof(p), "%s/%s", dir, cur.name[i]);
            unlink(p);
        }
    }
    folder_free(&cur);
    return 0;
}

/* --- PSP side ---------------------------------------------------------------- */

typedef struct { Folder *f; SyncConfig *cfg; const char *dir; int err; } PspRead;

static int psp_read_entry(void *u, const BridgeEntry *e) {
    PspRead *r = u;
    if (e->type != BRIDGE_TYPE_FILE || is_temp(e->name) || strlen(e->name) >= 128) return 0;
    if (r->f->n >= MAX_FILES) return 1;
    int i = r->f->n;
    snprintf(r->f->name[i], 128, "%s", e->name);
    r->f->n++;
    return 0;
}

/* 1 = read, 0 = no such folder, < 0 = error */
static int psp_read(SyncConfig *cfg, const char *folder, Folder *f) {
    char dir[512];
    memset(f, 0, sizeof(*f));
    snprintf(dir, sizeof(dir), PSP_SAVEDATA "/%s", folder);
    PspRead r = { f, cfg, dir, 0 };
    int st = cfg->psp.list(cfg->psp.ctx, dir, psp_read_entry, &r);
    if (st == BRIDGE_ERR_NOENT) return 0;
    if (st != BRIDGE_OK) return st;
    for (int i = 0; i < f->n; i++) {
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", dir, f->name[i]);
        st = cfg->psp.read_file(cfg->psp.ctx, p, &f->data[i], &f->len[i]);
        if (st != BRIDGE_OK) {
            folder_free(f);
            return st;
        }
    }
    folder_sort(f);
    return 1;
}

/* Makes the PSP folder exactly `f`: each file written under a temporary
   name then renamed (the old one removed first: FAT renames never replace),
   extra files removed, then everything read back and compared. */
static int psp_write(SyncConfig *cfg, const char *folder, const Folder *f) {
    char dir[512];
    snprintf(dir, sizeof(dir), PSP_SAVEDATA "/%s", folder);
    int st = cfg->psp.mkdir(cfg->psp.ctx, dir);
    if (st != BRIDGE_OK) return st;
    Folder old;
    int had = psp_read(cfg, folder, &old);
    if (had < 0) return had;
    for (int i = 0; i < f->n; i++) {
        char p[512], t[520];
        snprintf(p, sizeof(p), "%s/%s", dir, f->name[i]);
        snprintf(t, sizeof(t), "%s.pbtmp", p);
        cfg->psp.remove(cfg->psp.ctx, t);
        if ((st = cfg->psp.write_file(cfg->psp.ctx, t, f->data[i], f->len[i])) != BRIDGE_OK) goto out;
        int exists = 0;
        for (int j = 0; j < old.n && !exists; j++) exists = !strcmp(old.name[j], f->name[i]);
        if (exists && (st = cfg->psp.remove(cfg->psp.ctx, p)) != BRIDGE_OK) goto out;
        if ((st = cfg->psp.rename(cfg->psp.ctx, t, p)) != BRIDGE_OK) goto out;
    }
    for (int j = 0; j < old.n; j++) {
        int keep = 0;
        for (int i = 0; i < f->n && !keep; i++) keep = !strcmp(old.name[j], f->name[i]);
        if (!keep) {
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", dir, old.name[j]);
            cfg->psp.remove(cfg->psp.ctx, p);
        }
    }
    /* verify */
    Folder back;
    st = psp_read(cfg, folder, &back);
    if (st == 1) {
        st = folder_hash(&back) == folder_hash(f) ? BRIDGE_OK : BRIDGE_CLIENT_ERR_REPLY;
        folder_free(&back);
    } else if (st == 0) {
        st = BRIDGE_CLIENT_ERR_REPLY;
    }
out:
    if (had == 1) folder_free(&old);
    return st;
}

/* --- backups ------------------------------------------------------------------ */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void prune(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[512];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 512)
        if (de->d_name[0] != '.') names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_str);
    for (int i = 0; i < n - keep; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        DIR *b = opendir(p);
        if (b) {
            while ((de = readdir(b)))
                if (de->d_name[0] != '.' || strlen(de->d_name) > 2) {
                    char q[2300];
                    snprintf(q, sizeof(q), "%s/%s", p, de->d_name);
                    unlink(q);
                }
            closedir(b);
        }
        rmdir(p);
    }
    for (int i = 0; i < n; i++) free(names[i]);
}

static int backup(SyncConfig *cfg, const char *folder, const Folder *f, const char *from) {
    char base[1400], dir[1500];
    /* date to the millisecond: names sort in time order (prune relies on it) */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long ms = (long)(tv.tv_usec / 1000);
    time_t t = tv.tv_sec;
    snprintf(base, sizeof(base), "%s/%s", cfg->backupDir, folder);
    for (int k = 0; k < 1000; k++) {
        struct tm tm;
        char stamp[64];
        localtime_r(&t, &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
        snprintf(dir, sizeof(dir), "%s/%s.%03ld-%s", base, stamp, ms, from);
        if (access(dir, F_OK) != 0) break;
        if (++ms == 1000) { ms = 0; t++; }
    }
    if (mkdirs(dir)) return -1;
    for (int i = 0; i < f->n; i++) {
        char p[2048];
        snprintf(p, sizeof(p), "%s/%s", dir, f->name[i]);
        if (write_all(p, f->data[i], f->len[i])) return -1;
    }
    prune(base, cfg->keepBackups > 0 ? cfg->keepBackups : 20);
    fprintf(stderr, "pspbridged: backup of %s (%s version): %s\n", folder, from, dir);
    return 0;
}

/* --- state -------------------------------------------------------------------- */

static struct { char name[128]; uint64_t synced, seen; int hasSynced, hasSeen; } g_state[MAX_FOLDERS];
static int g_nState = 0;

void sync_reset(void) {
    g_nState = 0;
}

static int state_of(const char *name) {
    for (int i = 0; i < g_nState; i++)
        if (!strcmp(g_state[i].name, name)) return i;
    if (g_nState >= MAX_FOLDERS) return -1;
    memset(&g_state[g_nState], 0, sizeof(g_state[0]));
    snprintf(g_state[g_nState].name, 128, "%s", name);
    return g_nState++;
}

/* folder names starting with the game id, on either side */
typedef struct { char names[MAX_FOLDERS][128]; int n; const char *id; } Names;

static void add_name(Names *ns, const char *name) {
    if (strncmp(name, ns->id, strlen(ns->id)) != 0 || strlen(name) >= 128) return;
    for (int i = 0; i < ns->n; i++)
        if (!strcmp(ns->names[i], name)) return;
    if (ns->n < MAX_FOLDERS) snprintf(ns->names[ns->n++], 128, "%s", name);
}

static int psp_name(void *u, const BridgeEntry *e) {
    if (e->type == BRIDGE_TYPE_DIR) add_name(u, e->name);
    return 0;
}

static void mac_names(const char *dir, Names *ns) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        char p[2048];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (de->d_name[0] != '.' && stat(p, &st) == 0 && S_ISDIR(st.st_mode)) add_name(ns, de->d_name);
    }
    closedir(d);
}

/* --- the two operations ---------------------------------------------------------- */

int sync_game_start(SyncConfig *cfg, const char *gameId) {
    Names ns;
    memset(&ns, 0, sizeof(ns));
    ns.id = gameId;
    if (!gameId || !gameId[0]) return 0;
    int st = cfg->psp.list(cfg->psp.ctx, PSP_SAVEDATA, psp_name, &ns);
    if (st != BRIDGE_OK && st != BRIDGE_ERR_NOENT) return st;
    mac_names(cfg->macSaveDir, &ns);

    int copied = 0;
    for (int i = 0; i < ns.n; i++) {
        const char *name = ns.names[i];
        char macDir[2048];
        snprintf(macDir, sizeof(macDir), "%s/%s", cfg->macSaveDir, name);
        Folder p, m;
        int hasP = psp_read(cfg, name, &p);
        if (hasP < 0) return hasP;
        mac_read(macDir, &m);
        int hasM = m.n > 0;
        int s = state_of(name);
        if (hasP && hasM && folder_hash(&p) == folder_hash(&m)) {
            /* already the same */
        } else if (hasP) {
            /* the PSP wins; PPSSPP's version is kept */
            if (hasM) backup(cfg, name, &m, "ppsspp");
            if (mac_write(macDir, &p)) {
                fprintf(stderr, "pspbridged: cannot write %s\n", macDir);
            } else {
                fprintf(stderr, "pspbridged: save %s: PSP -> PPSSPP\n", name);
                copied++;
            }
        } else if (hasM) {
            st = psp_write(cfg, name, &m);
            if (st != BRIDGE_OK) {
                folder_free(&m);
                return st;
            }
            fprintf(stderr, "pspbridged: save %s: PPSSPP -> PSP (new on the PSP)\n", name);
            copied++;
        }
        if (s >= 0) {
            Folder now;
            mac_read(macDir, &now);
            g_state[s].synced = folder_hash(&now);
            g_state[s].hasSynced = 1;
            g_state[s].hasSeen = 0;
            folder_free(&now);
        }
        if (hasP == 1) folder_free(&p);
        folder_free(&m);
    }
    return copied;
}

int sync_push_changes(SyncConfig *cfg, const char *gameId) {
    Names ns;
    memset(&ns, 0, sizeof(ns));
    ns.id = gameId;
    if (!gameId || !gameId[0]) return 0;
    mac_names(cfg->macSaveDir, &ns);
    int pushed = 0;
    for (int i = 0; i < ns.n; i++) {
        const char *name = ns.names[i];
        char macDir[2048];
        snprintf(macDir, sizeof(macDir), "%s/%s", cfg->macSaveDir, name);
        Folder m;
        mac_read(macDir, &m);
        if (m.n == 0) continue;
        int s = state_of(name);
        if (s < 0) {
            folder_free(&m);
            continue;
        }
        uint64_t h = folder_hash(&m);
        int changed = !g_state[s].hasSynced || h != g_state[s].synced;
        int stable = g_state[s].hasSeen && g_state[s].seen == h;
        g_state[s].seen = h;
        g_state[s].hasSeen = 1;
        if (changed && stable) {
            Folder p;
            int hasP = psp_read(cfg, name, &p);
            if (hasP < 0) {
                folder_free(&m);
                return hasP;
            }
            if (hasP && folder_hash(&p) != h) backup(cfg, name, &p, "psp");
            int st = (hasP && folder_hash(&p) == h) ? BRIDGE_OK : psp_write(cfg, name, &m);
            if (hasP == 1) folder_free(&p);
            if (st != BRIDGE_OK) {
                fprintf(stderr, "pspbridged: save %s: PSP write failed (%s), will retry\n", name,
                        bridge_strerror(st));
                folder_free(&m);
                return st;
            }
            g_state[s].synced = h;
            g_state[s].hasSynced = 1;
            fprintf(stderr, "pspbridged: save %s: PPSSPP -> PSP, verified\n", name);
            pushed++;
        }
        folder_free(&m);
    }
    return pushed;
}
