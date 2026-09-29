/*
 * PSP Bridge — Memory Stick access for the file channel commands (BridgeFs,
 * see ../common/bridge_proto.h). Called from the bridge thread only.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>
#include "psp_fs.h"

/* errors as returned by the IO manager */
#define SCE_ENOENT 0x80010002
#define SCE_EEXIST 0x80010011

/* ISO reads come in a row from the same file: keep it open */
static SceUID g_fd = -1;
static char g_fdPath[BRIDGE_PATH_MAX];

/* Big structures off the thread's stack */
static SceIoDirent g_de;
static BridgeEntry g_entry;

static int err_of(int r)
{
	return ((unsigned int)r == SCE_ENOENT) ? BRIDGE_ERR_NOENT : BRIDGE_ERR_IO;
}

static int is_root(const char *path)
{
	return strcmp(path, "ms0:/") == 0 || strcmp(path, "ms0:") == 0;
}

static int fs_stat(void *ctx, const char *path, BridgeStat *st)
{
	SceIoStat s;
	int r;

	memset(&s, 0, sizeof(s));
	r = sceIoGetstat(path, &s);
	if (r < 0) {
		/* the root has no directory entry of its own */
		if (is_root(path)) {
			st->type = BRIDGE_TYPE_DIR;
			st->size = 0;
			return 0;
		}
		return err_of(r);
	}
	st->type = FIO_S_ISDIR(s.st_mode) ? BRIDGE_TYPE_DIR : BRIDGE_TYPE_FILE;
	st->size = st->type == BRIDGE_TYPE_FILE ? (uint64_t)s.st_size : 0;
	return 0;
}

static int fs_list(void *ctx, const char *path, BridgeListFn fn, void *user)
{
	SceUID d = sceIoDopen(path);
	if (d < 0)
		return err_of(d);
	/* d_private must be NULL or the FAT driver writes through it */
	memset(&g_de, 0, sizeof(g_de));
	while (sceIoDread(d, &g_de) > 0) {
		if (strcmp(g_de.d_name, ".") != 0 && strcmp(g_de.d_name, "..") != 0) {
			memset(&g_entry, 0, sizeof(g_entry));
			g_entry.type = FIO_S_ISDIR(g_de.d_stat.st_mode) ? BRIDGE_TYPE_DIR : BRIDGE_TYPE_FILE;
			g_entry.size = g_entry.type == BRIDGE_TYPE_FILE ? (uint64_t)g_de.d_stat.st_size : 0;
			strncpy(g_entry.name, g_de.d_name, sizeof(g_entry.name) - 1);
			if (fn(user, &g_entry))
				break;
		}
		memset(&g_de, 0, sizeof(g_de));
	}
	sceIoDclose(d);
	return 0;
}

static int fs_read(void *ctx, const char *path, uint64_t offset, uint8_t *dst, uint32_t len)
{
	int n;

	if (g_fd < 0 || strcmp(g_fdPath, path) != 0) {
		psp_fs_close();
		g_fd = sceIoOpen(path, PSP_O_RDONLY, 0);
		if (g_fd < 0) {
			int r = g_fd;
			g_fd = -1;
			return err_of(r);
		}
		strncpy(g_fdPath, path, sizeof(g_fdPath) - 1);
		g_fdPath[sizeof(g_fdPath) - 1] = 0;
	}
	if (sceIoLseek(g_fd, (SceOff)offset, PSP_SEEK_SET) < 0) {
		psp_fs_close();
		return BRIDGE_ERR_IO;
	}
	n = len ? sceIoRead(g_fd, dst, len) : 0;
	if (n < 0) {
		psp_fs_close();
		return err_of(n);
	}
	return n;
}

/* Writes (paths already limited to ms0:/PSP/SAVEDATA/ by the protocol) */
static int fs_write(void *ctx, const char *path, uint64_t offset, const uint8_t *src,
                    uint32_t len, int truncate)
{
	SceUID fd;
	int n = 0;

	if (g_fd >= 0 && strcmp(g_fdPath, path) == 0)
		psp_fs_close();
	fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | (truncate ? PSP_O_TRUNC : 0), 0777);
	if (fd < 0)
		return err_of(fd);
	if (sceIoLseek(fd, (SceOff)offset, PSP_SEEK_SET) < 0) {
		sceIoClose(fd);
		return BRIDGE_ERR_IO;
	}
	if (len)
		n = sceIoWrite(fd, src, len);
	sceIoClose(fd);
	if (n < 0 || (uint32_t)n != len)
		return BRIDGE_ERR_IO; /* Memory Stick full? */
	return n;
}

static int fs_mkdir(void *ctx, const char *path)
{
	int r = sceIoMkdir(path, 0777);
	if (r < 0 && (unsigned int)r != SCE_EEXIST)
		return err_of(r);
	return 0;
}

static int fs_rename(void *ctx, const char *from, const char *to)
{
	SceIoStat s;
	int r;
	psp_fs_close();
	memset(&s, 0, sizeof(s));
	if (sceIoGetstat(to, &s) >= 0)
		return BRIDGE_ERR_EXIST;
	r = sceIoRename(from, to);
	return r < 0 ? err_of(r) : 0;
}

static int fs_remove(void *ctx, const char *path)
{
	int r;
	psp_fs_close();
	r = sceIoRemove(path);
	return r < 0 ? err_of(r) : 0;
}

void psp_fs_close(void)
{
	if (g_fd >= 0)
		sceIoClose(g_fd);
	g_fd = -1;
	g_fdPath[0] = 0;
}

const BridgeFs *psp_fs(void)
{
	static const BridgeFs fs = { fs_stat, fs_list, fs_read, fs_write, fs_mkdir,
	                             fs_rename, fs_remove, NULL };
	return &fs;
}
