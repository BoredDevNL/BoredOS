// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef FAT32_H
#define FAT32_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media_descriptor;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t sectors_per_fat_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved2;
    uint8_t boot_signature;
    uint32_t serial_number;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
    uint8_t boot_code[420];
    uint16_t boot_signature_value;
} __attribute__((packed)) fat32_boot_sector_t;
typedef fat32_boot_sector_t FAT32_BootSector;

typedef struct {
    uint8_t filename[8];
    uint8_t extension[3];
    uint8_t attributes;
    uint8_t reserved;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t start_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t start_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;
typedef fat32_dir_entry_t FAT32_DirEntry;

typedef struct {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster;
    uint16_t name3[2];
} __attribute__((packed)) fat32_lfn_entry_t;
typedef fat32_lfn_entry_t FAT32_LFNEntry;

#define ATTR_READ_ONLY   0x01
#define ATTR_HIDDEN      0x02
#define ATTR_SYSTEM      0x04
#define ATTR_VOLUME_ID   0x08
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20
#define ATTR_DEVICE      0x40
#define ATTR_RESERVED    0x80
#define ATTR_LFN         0x0F

#define FAT32_SECTOR_SIZE 512
#define FAT32_CLUSTER_SIZE 4096
#define FAT32_MAX_FILENAME 256
#define FAT32_MAX_PATH 1024
#define FAT32_ROOT_CLUSTER 2

typedef struct {
    uint32_t cluster;
    uint32_t start_cluster;
    uint32_t position;
    uint32_t size;
    uint32_t mode;
    bool valid;
    uint32_t dir_sector;
    uint32_t dir_offset;
    bool is_directory;
    uint8_t attributes;
    void *volume;
} fat32_file_handle_t;

typedef fat32_file_handle_t FAT32_FileHandle;

typedef struct {
    char name[FAT32_MAX_FILENAME];
    uint32_t size;
    bool is_directory;
    uint32_t start_cluster;
    uint16_t write_date;
    uint16_t write_time;
} fat32_file_info_t;

typedef fat32_file_info_t FAT32_FileInfo;

struct vfs_fs_ops;

struct vfs_fs_ops *fat32_get_ops(void);
struct vfs_fs_ops *fat32_get_realfs_ops(void);

void *fat32_mount_volume(void *disk_ptr);

void fat32_init(void);

fat32_file_handle_t *fat32_open(const char *path, const char *mode);
void fat32_close(fat32_file_handle_t *handle);
int fat32_read(fat32_file_handle_t *handle, void *buffer, size_t size);
int fat32_write(fat32_file_handle_t *handle, const void *buffer, size_t size);
int fat32_seek(fat32_file_handle_t *handle, int offset, int whence);

bool fat32_mkdir(const char *path);
void fat32_mkdir_recursive(const char *path);
bool fat32_rmdir(const char *path);
bool fat32_delete(const char *path);
bool fat32_exists(const char *path);
bool fat32_rename(const char *old_path, const char *new_path);
bool fat32_is_directory(const char *path);

int fat32_list_directory(const char *path, fat32_file_info_t *entries, int max_entries);
int fat32_get_info(const char *path, fat32_file_info_t *info);

void fat32_normalize_path(const char *path, char *normalized);
void fat32_set_root_volume(void *fs_private);

#endif // FAT32_H
