// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Abdelkader Boudih

#include "ext4_config.h"

#include "ext4fs.h"
#include "disk.h"
#include "memory_manager.h"
#include "spinlock.h"
#include <stddef.h>
#include <string.h>

#include <ext4.h>
#include <ext4_blockdev.h>

extern void serial_write(const char *str);
extern void serial_write_hex(uint64_t value);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    uint8_t *b = (uint8_t *)base;
    uint8_t tmp[64]; // ext4_dx_sort_entry is 8 bytes
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(b + (j - 1) * size, b + j * size) > 0) {
            memcpy(tmp, b + j * size, size);
            memcpy(b + j * size, b + (j - 1) * size, size);
            memcpy(b + (j - 1) * size, tmp, size);
            j--;
        }
    }
}

void *ext4_user_malloc(size_t size) {
    return kmalloc(size);
}

void *ext4_user_calloc(size_t n, size_t size) {
    return kcalloc(n, size);
}

void ext4_user_free(void *ptr) {
    kfree(ptr);
}

typedef struct {
    struct ext4_blockdev bdev;
    struct ext4_blockdev_iface iface;
    uint8_t ph_buf[512];
    struct ext4_bcache bcache;
    Disk *disk;
    char dev_name[32];
    char mount_point[32];
    spinlock_t lock;
} ext4fs_vol_t;

static int bdev_open(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

static int bdev_bread(struct ext4_blockdev *bdev, void *buf,
                      uint64_t blk_id, uint32_t blk_cnt) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)bdev;
    Disk *d = vol->disk;

    if (d->read_sectors) {
        return d->read_sectors(d, (uint32_t)blk_id, blk_cnt,
                               (uint8_t *)buf) == 0 ? EOK : EIO;
    }
    for (uint32_t i = 0; i < blk_cnt; i++) {
        if (d->read_sector(d, (uint32_t)(blk_id + i),
                           (uint8_t *)buf + i * 512) != 0)
            return EIO;
    }
    return EOK;
}

static int bdev_bwrite(struct ext4_blockdev *bdev, const void *buf,
                       uint64_t blk_id, uint32_t blk_cnt) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)bdev;
    Disk *d = vol->disk;

    if (d->write_sectors) {
        return d->write_sectors(d, (uint32_t)blk_id, blk_cnt,
                                (const uint8_t *)buf) == 0 ? EOK : EIO;
    }
    for (uint32_t i = 0; i < blk_cnt; i++) {
        if (d->write_sector(d, (uint32_t)(blk_id + i),
                            (const uint8_t *)buf + i * 512) != 0)
            return EIO;
    }
    return EOK;
}

static int bdev_close(struct ext4_blockdev *bdev) {
    (void)bdev;
    return EOK;
}

static int bdev_lock(struct ext4_blockdev *bdev) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)bdev;
    spinlock_acquire(&vol->lock);
    return EOK;
}

static int bdev_unlock(struct ext4_blockdev *bdev) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)bdev;
    spinlock_release(&vol->lock);
    return EOK;
}

static int vol_counter = 0;

void* ext4fs_mount_volume(void *disk_ptr) {
    Disk *d = (Disk *)disk_ptr;
    if (!d || !d->read_sector) return NULL;

    ext4fs_vol_t *vol = (ext4fs_vol_t *)kcalloc(1, sizeof(ext4fs_vol_t));
    if (!vol) return NULL;

    vol->disk = d;
    vol->lock = SPINLOCK_INIT;

    vol->iface.open   = bdev_open;
    vol->iface.bread  = bdev_bread;
    vol->iface.bwrite = bdev_bwrite;
    vol->iface.close  = bdev_close;
    vol->iface.lock   = bdev_lock;
    vol->iface.unlock = bdev_unlock;
    vol->iface.ph_bsize = 512;
    vol->iface.ph_bcnt  = d->total_sectors;
    vol->iface.ph_bbuf  = vol->ph_buf;

    vol->bdev.bdif = &vol->iface;
    vol->bdev.part_offset = 0;
    vol->bdev.part_size = (uint64_t)d->total_sectors * 512;

    int id = vol_counter++;
    strcpy(vol->dev_name, "ext4_");
    vol->dev_name[5] = '0' + (id % 10);
    vol->dev_name[6] = '\0';

    vol->mount_point[0] = '/';
    strcpy(vol->mount_point + 1, vol->dev_name);
    int len = strlen(vol->mount_point);
    vol->mount_point[len] = '/';
    vol->mount_point[len + 1] = '\0';

    int r = ext4_device_register(&vol->bdev, vol->dev_name);
    if (r != EOK) {
        serial_write("[EXT4] device_register failed\n");
        kfree(vol);
        return NULL;
    }

    r = ext4_mount(vol->dev_name, vol->mount_point, false);
    if (r != EOK) {
        serial_write("[EXT4] mount failed for ");
        serial_write(d->devname);
        serial_write("\n");
        ext4_device_unregister(vol->dev_name);
        kfree(vol);
        return NULL;
    }

    r = ext4_recover(vol->mount_point);
    if (r != EOK && r != ENOTSUP) {
        serial_write("[EXT4] journal recovery failed\n");
    }

    ext4_journal_start(vol->mount_point);
    ext4_cache_write_back(vol->mount_point, true);

    serial_write("[EXT4] Mounted ");
    serial_write(d->devname);
    serial_write(" at ");
    serial_write(vol->mount_point);
    serial_write("\n");

    return vol;
}

void ext4fs_umount_volume(void *fs_private) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    if (!vol) return;

    ext4_cache_write_back(vol->mount_point, false);
    ext4_journal_stop(vol->mount_point);
    ext4_umount(vol->mount_point);
    ext4_device_unregister(vol->dev_name);
    kfree(vol);
}

static void ext4fs_build_path(ext4fs_vol_t *vol, const char *rel,
                              char *out, int out_size) {
    int mlen = strlen(vol->mount_point);
    if (mlen >= out_size - 1) { out[0] = '\0'; return; }
    strcpy(out, vol->mount_point);
    while (*rel == '/') rel++;
    int rlen = strlen(rel);
    if (mlen + rlen >= out_size - 1) { out[0] = '\0'; return; }
    strcpy(out + mlen, rel);
}

/* Ensure path ends with a trailing slash so lwext4 treats it as a directory. */
static void ext4fs_ensure_trailing_slash(char *path) {
    int plen = strlen(path);
    if (plen > 0 && path[plen - 1] != '/') {
        path[plen] = '/';
        path[plen + 1] = '\0';
    }
}

typedef struct {
    ext4_file file;
    bool valid;
} ext4fs_handle_t;

static void* vfs_ext4_open(void *fs_private, const char *rel_path,
                           const char *mode) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));

    ext4fs_handle_t *h = (ext4fs_handle_t *)kcalloc(1, sizeof(ext4fs_handle_t));
    if (!h) return NULL;

    int r = ext4_fopen(&h->file, path, mode);
    if (r != EOK) {
        kfree(h);
        return NULL;
    }
    h->valid = true;
    return h;
}

static void vfs_ext4_close(void *fs_private, void *file_handle) {
    (void)fs_private;
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h) return;
    if (h->valid) ext4_fclose(&h->file);
    kfree(h);
}

static int vfs_ext4_read(void *fs_private, void *file_handle,
                         void *buf, int size) {
    (void)fs_private;
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h || !h->valid) return -1;

    size_t rcnt = 0;
    int r = ext4_fread(&h->file, buf, (size_t)size, &rcnt);
    if (r != EOK && rcnt == 0) return -1;
    return (int)rcnt;
}

static int vfs_ext4_write(void *fs_private, void *file_handle,
                          const void *buf, int size) {
    (void)fs_private;
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h || !h->valid) return -1;

    size_t wcnt = 0;
    int r = ext4_fwrite(&h->file, buf, (size_t)size, &wcnt);
    if (r != EOK && wcnt == 0) return -1;
    return (int)wcnt;
}

static int vfs_ext4_seek(void *fs_private, void *file_handle,
                         int offset, int whence) {
    (void)fs_private;
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h || !h->valid) return -1;

    return ext4_fseek(&h->file, (int64_t)offset, (uint32_t)whence) == EOK ? 0 : -1;
}

static uint32_t vfs_ext4_get_position(void *file_handle) {
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h || !h->valid) return 0;
    return (uint32_t)ext4_ftell(&h->file);
}

static uint32_t vfs_ext4_get_size(void *file_handle) {
    ext4fs_handle_t *h = (ext4fs_handle_t *)file_handle;
    if (!h || !h->valid) return 0;
    return (uint32_t)ext4_fsize(&h->file);
}

static int vfs_ext4_readdir(void *fs_private, const char *rel_path,
                            vfs_dirent_t *entries, int max, int offset) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));
    ext4fs_ensure_trailing_slash(path);

    ext4_dir dir;
    int r = ext4_dir_open(&dir, path);
    if (r != EOK) return -1;

    int count = 0;
    int skip = offset;
    const ext4_direntry *de;

    while ((de = ext4_dir_entry_next(&dir)) != NULL) {
        if (de->inode == 0) continue;
        if (de->name_length == 1 && de->name[0] == '.') continue;
        if (de->name_length == 2 && de->name[0] == '.' && de->name[1] == '.')
            continue;

        if (skip > 0) { skip--; continue; }
        if (count >= max) break;

        int nlen = de->name_length;
        if (nlen >= VFS_MAX_NAME) nlen = VFS_MAX_NAME - 1;
        memcpy(entries[count].name, de->name, nlen);
        entries[count].name[nlen] = '\0';

        entries[count].is_directory = (de->inode_type == 2);
        entries[count].size = 0;
        entries[count].start_cluster = de->inode;
        entries[count].write_date = 0;
        entries[count].write_time = 0;
        count++;
    }

    ext4_dir_close(&dir);
    return count;
}

static bool vfs_ext4_mkdir(void *fs_private, const char *rel_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));
    return ext4_dir_mk(path) == EOK;
}

static bool vfs_ext4_rmdir(void *fs_private, const char *rel_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));
    return ext4_dir_rm(path) == EOK;
}

static bool vfs_ext4_unlink(void *fs_private, const char *rel_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));
    return ext4_fremove(path) == EOK;
}

static bool vfs_ext4_rename(void *fs_private, const char *old_path,
                            const char *new_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char opath[512], npath[512];
    ext4fs_build_path(vol, old_path, opath, sizeof(opath));
    ext4fs_build_path(vol, new_path, npath, sizeof(npath));
    return ext4_frename(opath, npath) == EOK;
}

static bool vfs_ext4_exists(void *fs_private, const char *rel_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));

    if (ext4_inode_exist(path, 1) == EOK) return true;
    ext4fs_ensure_trailing_slash(path);
    if (ext4_inode_exist(path, 2) == EOK) return true;
    return false;
}

static bool vfs_ext4_is_dir(void *fs_private, const char *rel_path) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));
    ext4fs_ensure_trailing_slash(path);
    return ext4_inode_exist(path, 2) == EOK;
}

static int vfs_ext4_get_info(void *fs_private, const char *rel_path,
                             vfs_dirent_t *info) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    char path[512];
    ext4fs_build_path(vol, rel_path, path, sizeof(path));

    const char *name = rel_path;
    const char *p = rel_path;
    while (*p) { if (*p == '/') name = p + 1; p++; }
    if (*name == '\0') name = rel_path;

    int nlen = strlen(name);
    if (nlen >= VFS_MAX_NAME) nlen = VFS_MAX_NAME - 1;
    memcpy(info->name, name, nlen);
    info->name[nlen] = '\0';
    info->start_cluster = 0;
    info->write_date = 0;
    info->write_time = 0;

    char dpath[512];
    strcpy(dpath, path);
    ext4fs_ensure_trailing_slash(dpath);
    if (ext4_inode_exist(dpath, 2) == EOK) {
        info->is_directory = 1;
        info->size = 0;
        return 0;
    }

    ext4_file f;
    int r = ext4_fopen(&f, path, "r");
    if (r != EOK) return -1;

    info->is_directory = 0;
    info->size = (uint32_t)ext4_fsize(&f);
    ext4_fclose(&f);
    return 0;
}

static int vfs_ext4_statfs(void *fs_private, vfs_statfs_t *stat) {
    ext4fs_vol_t *vol = (ext4fs_vol_t *)fs_private;
    struct ext4_mount_stats ms;

    int r = ext4_mount_point_stats(vol->mount_point, &ms);
    if (r != EOK) return -1;

    stat->block_size = ms.block_size;
    stat->total_blocks = ms.blocks_count;
    stat->free_blocks = ms.free_blocks_count;
    return 0;
}

static vfs_fs_ops_t ext4_ops = {
    .open       = vfs_ext4_open,
    .close      = vfs_ext4_close,
    .read       = vfs_ext4_read,
    .write      = vfs_ext4_write,
    .seek       = vfs_ext4_seek,
    .readdir    = vfs_ext4_readdir,
    .mkdir      = vfs_ext4_mkdir,
    .rmdir      = vfs_ext4_rmdir,
    .unlink     = vfs_ext4_unlink,
    .rename     = vfs_ext4_rename,
    .exists     = vfs_ext4_exists,
    .is_dir     = vfs_ext4_is_dir,
    .get_info   = vfs_ext4_get_info,
    .get_position = vfs_ext4_get_position,
    .get_size   = vfs_ext4_get_size,
    .statfs     = vfs_ext4_statfs,
    .poll       = NULL,
    .ioctl      = NULL,
};

vfs_fs_ops_t* ext4fs_get_ops(void) {
    return &ext4_ops;
}
