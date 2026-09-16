#ifndef D_MODULE_DATASTORE_LINUX_H
#define D_MODULE_DATASTORE_LINUX_H

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------
//
// d_module_datastore_linux.h - a keyed blob store on a filesystem. The linux half of the seam
// d_module_datastore_esp32.h fills with NVS.
//
// One directory, one file per key, values opaque. It is the BOTTOM layer: it knows nothing about
// what it holds, only how to get bytes onto something that survives a restart and back again.
//
// WRITES ARE ATOMIC OR THEY ARE NOTHING. Write to a temporary, fsync it, rename over the target,
// then fsync the DIRECTORY so the rename itself is durable. Without that last step the rename can
// be lost on power failure even though the data was flushed, and the store comes back with the old
// value while believing it wrote the new one -- the exact failure this layer exists to prevent.
//
// IT CHECKS THAT IT CAN ACTUALLY PERSIST, and says so loudly when it cannot. /run and /dev/shm are
// tmpfs: open, write and fsync all succeed and the data is gone on the next boot. On a node with a
// read-only overlay root an ordinary path fails the same way, silently, in the RAM upper layer.
// Silent non-persistence is worse than none -- a node believes its sequence is protected and it is
// not -- so datastore_persistent() reports it and the caller is expected to complain.
//
// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

/* POSIX.1-2008, but hidden under a strict -std=c11 without _GNU_SOURCE. It is advisory here -- the
   directory was created a few lines earlier -- so falling back to 0 costs a check, not a guarantee. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#ifndef DATASTORE_KEY_MAX
#define DATASTORE_KEY_MAX 32
#endif
#ifndef DATASTORE_PATH_MAX
#define DATASTORE_PATH_MAX 256
#endif
#ifndef DATASTORE_BLOB_MAX
#define DATASTORE_BLOB_MAX 8192
#endif

/* The two filesystems that accept a write and lose it on reboot. Spelled out rather than pulled
   from <linux/magic.h>, which is not present on every toolchain this builds under. */
#define _DATASTORE_TMPFS_MAGIC 0x01021994L
#define _DATASTORE_RAMFS_MAGIC 0x858458f6L

typedef struct {
    char dir[DATASTORE_PATH_MAX];
    bool open;
    bool persistent; /* false: the bytes will not survive a reboot -- see the header comment */
    uint32_t stat_read, stat_write, stat_read_fail, stat_write_fail;
} datastore_t;

// ------------------------------------------------------------------------------------------------------------------------

static inline bool _datastore_path(const datastore_t *const ds, const char *const key, const char *const suffix, char *const out, const size_t size) {
    if (key == NULL || key[0] == '\0' || strlen(key) >= DATASTORE_KEY_MAX)
        return false;
    for (const char *c = key; *c != '\0'; c++) /* a key is a filename: no traversal, no surprises */
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.'))
            return false;
    return snprintf(out, size, "%s/%s%s", ds->dir, key, suffix) < (int)size;
}

static inline bool datastore_persistent(const datastore_t *const ds) {
    return ds != NULL && ds->open && ds->persistent;
}

/* `location` is a directory; it is created if it does not exist. */
static inline bool datastore_open(datastore_t *const ds, const char *const location) {
    if (ds == NULL || location == NULL)
        return false;
    *ds = (datastore_t){ 0 };
    if (snprintf(ds->dir, sizeof(ds->dir), "%s", location) >= (int)sizeof(ds->dir))
        return false;
    if (mkdir(ds->dir, 0700) != 0 && errno != EEXIST)
        return false;
    struct statfs sb;
    /* Unknown filesystem type is treated as persistent: this is a warning mechanism, and refusing
       to run on anything unrecognised would be worse than the problem it guards against. */
    ds->persistent = (statfs(ds->dir, &sb) != 0) || (sb.f_type != _DATASTORE_TMPFS_MAGIC && sb.f_type != _DATASTORE_RAMFS_MAGIC);
    ds->open = true;
    return true;
}

static inline void datastore_close(datastore_t *const ds) {
    if (ds != NULL)
        ds->open = false;
}

static inline bool datastore_read(datastore_t *const ds, const char *const key, void *const buf, const size_t size, size_t *const out_len) {
    char path[DATASTORE_PATH_MAX + DATASTORE_KEY_MAX + 8];
    if (ds == NULL || !ds->open || buf == NULL || !_datastore_path(ds, key, "", path, sizeof(path)))
        return false;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ds->stat_read_fail++;
        return false;
    }
    const ssize_t n = read(fd, buf, size);
    (void)close(fd);
    if (n < 0) {
        ds->stat_read_fail++;
        return false;
    }
    ds->stat_read++;
    if (out_len != NULL)
        *out_len = (size_t)n;
    return true;
}

static inline bool datastore_write(datastore_t *const ds, const char *const key, const void *const data, const size_t len) {
    char path[DATASTORE_PATH_MAX + DATASTORE_KEY_MAX + 8], temp[DATASTORE_PATH_MAX + DATASTORE_KEY_MAX + 8];
    if (ds == NULL || !ds->open || data == NULL || len > DATASTORE_BLOB_MAX)
        return false;
    if (!_datastore_path(ds, key, "", path, sizeof(path)) || !_datastore_path(ds, key, ".tmp", temp, sizeof(temp)))
        return false;
    const int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        ds->stat_write_fail++;
        return false;
    }
    bool ok = (write(fd, data, len) == (ssize_t)len) && (fsync(fd) == 0);
    ok = (close(fd) == 0) && ok;
    if (ok)
        ok = (rename(temp, path) == 0);
    if (!ok) {
        (void)unlink(temp);
        ds->stat_write_fail++;
        return false;
    }
    /* The rename is a directory operation, so the DIRECTORY is what has to be flushed for it to
       survive power loss. Skipping this is the classic way to lose a write that reported success. */
    const int dirfd = open(ds->dir, O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
    }
    ds->stat_write++;
    return true;
}

static inline bool datastore_erase(datastore_t *const ds, const char *const key) {
    char path[DATASTORE_PATH_MAX + DATASTORE_KEY_MAX + 8];
    if (ds == NULL || !ds->open || !_datastore_path(ds, key, "", path, sizeof(path)))
        return false;
    return unlink(path) == 0 || errno == ENOENT;
}

// ------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------

#endif /* D_MODULE_DATASTORE_LINUX_H */
