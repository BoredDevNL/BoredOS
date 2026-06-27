// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VFS_MAX_PATH 1024
#define VFS_MAX_NAME 256
#define VFS_MAX_MOUNTS 16
#define VFS_MAX_OPEN_FILES 64

#define POLLIN     0x0001
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020

struct poll_table;

// statfs structure
typedef struct {
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t block_size;
} vfs_statfs_t;

// Forward declarations
typedef struct vfs_mount vfs_mount_t;
typedef struct vfs_file vfs_file_t;

// Directory entry for readdir
typedef struct vfs_dirent {
    char name[VFS_MAX_NAME];
    uint32_t size;
    uint8_t is_directory;
    uint32_t start_cluster;
    uint16_t write_date;
    uint16_t write_time;
} vfs_dirent_t;

// Filesystem operations — implemented by each filesystem type
typedef struct vfs_fs_ops {
    // File operations — return opaque FS handle
    void* (*open)(void *fs_private, const char *rel_path, const char *mode);
    void  (*close)(void *fs_private, void *file_handle);
    int   (*read)(void *fs_private, void *file_handle, void *buf, int size);
    int   (*write)(void *fs_private, void *file_handle, const void *buf, int size);
    int   (*seek)(void *fs_private, void *file_handle, int offset, int whence);

    // Directory operations
    int   (*readdir)(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset);
    bool  (*mkdir)(void *fs_private, const char *rel_path);
    bool  (*rmdir)(void *fs_private, const char *rel_path);
    bool  (*unlink)(void *fs_private, const char *rel_path);
    bool  (*rename)(void *fs_private, const char *old_path, const char *new_path);

    // Query operations
    bool  (*exists)(void *fs_private, const char *rel_path);
    bool  (*is_dir)(void *fs_private, const char *rel_path);
    int   (*get_info)(void *fs_private, const char *rel_path, vfs_dirent_t *info);
    int   (*statfs)(void *fs_private, vfs_statfs_t *stat);

    // Handle info (for backward compat with syscall position/size queries)
    uint32_t (*get_position)(void *file_handle);
    uint32_t (*get_size)(void *file_handle);
    int      (*poll)(void *fs_private, void *file_handle, struct poll_table *pt);
    int      (*ioctl)(void *fs_private, void *file_handle, uint64_t request, void *arg);
} vfs_fs_ops_t;

#define DEVICE_TYPE_BLOCK       0
#define DEVICE_TYPE_TTY         1
#define DEVICE_TYPE_KEYBOARD    2
#define DEVICE_TYPE_MOUSE       3
#define DEVICE_TYPE_FRAMEBUFFER 4
#define DEVICE_TYPE_SHM         5
#define DEVICE_TYPE_PCSPKR      6
#define DEVICE_TYPE_AUDIO       7
#define DEVICE_TYPE_MIXER       8

// VFS file handle
struct vfs_file {
    void *fs_handle;        // FS-specific handle (e.g. FAT32_FileHandle*)
    vfs_mount_t *mount;     // Mount this file belongs to
    bool valid;
    uint64_t position;      // Current Seek Position (for raw devices/fallbacks)
    bool is_device;         // Is this a raw device handle?
    int device_type;        // DEVICE_TYPE_BLOCK, TTY, etc.
};


// Mount entry
struct vfs_mount {
    char path[256];         // Mount point (e.g. "/", "/mnt/sda1")
    int path_len;
    vfs_fs_ops_t *ops;
    void *fs_private;       // FS-specific data (e.g. FAT32_Volume*)
    char device[32];        // Device name (e.g. "ramfs", "sda1")
    char fs_type[16];       // "ramfs", "fat32"
    bool active;
};

// Initialization
void vfs_init(void);

// Mount/unmount
bool vfs_mount(const char *mount_path, const char *device, const char *fs_type,
               vfs_fs_ops_t *ops, void *fs_private);
bool vfs_umount(const char *mount_path);

// File operations
vfs_file_t* vfs_open(const char *path, const char *mode);
void vfs_close(vfs_file_t *file);
int vfs_read(vfs_file_t *file, void *buf, int size);
int vfs_write(vfs_file_t *file, const void *buf, int size);
int vfs_seek(vfs_file_t *file, int offset, int whence);
int vfs_poll(vfs_file_t *file, struct poll_table *pt);

// Directory operations
int vfs_list_directory(const char *path, vfs_dirent_t *entries, int max, int offset);
bool vfs_mkdir(const char *path);
bool vfs_rmdir(const char *path);
bool vfs_delete(const char *path);
bool vfs_rename(const char *old_path, const char *new_path);

// Query operations
bool vfs_exists(const char *path);
bool vfs_is_directory(const char *path);
int vfs_get_info(const char *path, vfs_dirent_t *info);
int vfs_statfs(const char *path, vfs_statfs_t *stat);

// Mount enumeration
int vfs_get_mount_count(void);
vfs_mount_t* vfs_get_mount(int index);

// Block device auto-mount
void vfs_automount_partition(const char *devname);

// Path utilities
void vfs_normalize_path(const char *cwd, const char *path, char *normalized);

// Backward compat: get position/size from vfs_file
uint32_t vfs_file_position(vfs_file_t *file);
uint32_t vfs_file_size(vfs_file_t *file);

#endif
