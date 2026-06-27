// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

// === FAT32 Structures ===

// Boot Sector (512 bytes)
typedef struct {
    uint8_t jmp[3];                 // Jump instruction
    uint8_t oem[8];                 // OEM identifier
    uint16_t bytes_per_sector;      // Bytes per sector (usually 512)
    uint8_t sectors_per_cluster;    // Sectors per cluster
    uint16_t reserved_sectors;      // Reserved sectors (usually 1)
    uint8_t num_fats;               // Number of FATs (usually 2)
    uint16_t root_entries;          // Root directory entries (0 for FAT32)
    uint16_t total_sectors_16;      // Total sectors 16-bit (0 for FAT32)
    uint8_t media_descriptor;       // Media descriptor
    uint16_t sectors_per_fat_16;    // Sectors per FAT 16-bit (0 for FAT32)
    uint16_t sectors_per_track;     // Sectors per track
    uint16_t num_heads;             // Number of heads
    uint32_t hidden_sectors;        // Hidden sectors
    uint32_t total_sectors_32;      // Total sectors 32-bit

    // FAT32 Specific
    uint32_t sectors_per_fat_32;    // Sectors per FAT 32-bit
    uint16_t flags;                 // Flags
    uint16_t version;               // Version
    uint32_t root_cluster;          // Root directory cluster
    uint16_t fsinfo_sector;         // FSInfo sector number
    uint16_t backup_boot_sector;    // Backup boot sector number
    uint8_t reserved[12];           // Reserved
    uint8_t drive_number;           // Drive number
    uint8_t reserved2;              // Reserved
    uint8_t boot_signature;         // Boot signature
    uint32_t serial_number;         // Volume serial number
    uint8_t volume_label[11];       // Volume label
    uint8_t fs_type[8];             // Filesystem type ("FAT32   ")
    uint8_t boot_code[420];         // Boot code
    uint16_t boot_signature_value;  // Boot signature value (0xAA55)
} __attribute__((packed)) FAT32_BootSector;

// Directory Entry (32 bytes)
typedef struct {
    uint8_t filename[8];            // Filename (8 bytes)
    uint8_t extension[3];           // Extension (3 bytes)
    uint8_t attributes;             // File attributes
    uint8_t reserved;               // Reserved
    uint8_t creation_time_tenths;   // Creation time (tenths of second)
    uint16_t creation_time;         // Creation time (HH:MM:SS)
    uint16_t creation_date;         // Creation date (YYYY:MM:DD)
    uint16_t last_access_date;      // Last access date
    uint16_t start_cluster_high;    // Start cluster (high word)
    uint16_t write_time;            // Write time
    uint16_t write_date;            // Write date
    uint16_t start_cluster_low;     // Start cluster (low word)
    uint32_t file_size;             // File size
} __attribute__((packed)) FAT32_DirEntry;

// Long File Name Directory Entry (32 bytes)
typedef struct {
    uint8_t order;                  // Sequence number (0x40 = last, | index)
    uint16_t name1[5];              // Characters 1-5 (UCS-2)
    uint8_t attr;                   // Always 0x0F
    uint8_t type;                   // Always 0x00
    uint8_t checksum;               // Checksum of short name
    uint16_t name2[6];              // Characters 6-11 (UCS-2)
    uint16_t first_cluster;         // Always 0x0000
    uint16_t name3[2];              // Characters 12-13 (UCS-2)
} __attribute__((packed)) FAT32_LFNEntry;

// File Attributes
#define ATTR_READ_ONLY   0x01
#define ATTR_HIDDEN      0x02
#define ATTR_SYSTEM      0x04
#define ATTR_VOLUME_ID   0x08
#define ATTR_DIRECTORY   0x10
#define ATTR_ARCHIVE     0x20
#define ATTR_DEVICE      0x40
#define ATTR_RESERVED    0x80
#define ATTR_LFN         0x0F  // LFN marker (all of the above ORed)

// FAT32 Constants
#define FAT32_SECTOR_SIZE 512
#define FAT32_CLUSTER_SIZE 4096  // 8 sectors per cluster
#define FAT32_MAX_FILENAME 256
#define FAT32_MAX_PATH 1024
#define FAT32_ROOT_CLUSTER 2

// File Handle
typedef struct {
    uint32_t cluster;               // Current cluster
    uint32_t start_cluster;         // Start cluster (for file entry lookup)
    uint32_t position;              // Current position in file
    uint32_t size;                  // File size
    uint32_t mode;                  // 0=read, 1=write, 2=append
    bool valid;                     // Is this handle valid?
    uint32_t dir_sector;            // Sector containing the directory entry
    uint32_t dir_offset;            // Offset within that sector
    bool is_directory;              // Is this a directory?
    uint8_t attributes;             // File attributes
    void *volume;                   // Pointer to owning FAT32_Volume (or NULL for ramfs)
} FAT32_FileHandle;

// Directory Entry Info (for listing)
typedef struct {
    char name[FAT32_MAX_FILENAME];
    uint32_t size;
    bool is_directory;
    uint32_t start_cluster;
    uint16_t write_date;
    uint16_t write_time;
} FAT32_FileInfo;

// === VFS Integration ===
// Forward-declared VFS ops type (defined in vfs.h)
struct vfs_fs_ops;

// Get VFS ops structs for registration
struct vfs_fs_ops* fat32_get_ramfs_ops(void);
struct vfs_fs_ops* fat32_get_realfs_ops(void);

// Mount a real FAT32 volume from a block device — returns fs_private for VFS
void* fat32_mount_volume(void *disk_ptr);

// === Function Declarations ===

// Initialization
void fat32_init(void);

// File Operations (backward-compat wrappers — dispatch through VFS)
FAT32_FileHandle* fat32_open(const char *path, const char *mode);
void fat32_close(FAT32_FileHandle *handle);
int fat32_read(FAT32_FileHandle *handle, void *buffer, int size);
int fat32_write(FAT32_FileHandle *handle, const void *buffer, int size);
int fat32_seek(FAT32_FileHandle *handle, int offset, int whence);

// Directory Operations
bool fat32_mkdir(const char *path);
bool fat32_rmdir(const char *path);
bool fat32_delete(const char *path);
bool fat32_exists(const char *path);
bool fat32_rename(const char *old_path, const char *new_path);
bool fat32_is_directory(const char *path);

// Listing
int fat32_list_directory(const char *path, FAT32_FileInfo *entries, int max_entries);

// Info
int fat32_get_info(const char *path, FAT32_FileInfo *info);

// Working Directory (backward compat — wraps VFS path tracking)
bool fat32_chdir(const char *path);
void fat32_get_current_dir(char *buffer, int size);
bool fat32_change_drive(char drive);
char fat32_get_current_drive(void);

// Utilities
void fat32_normalize_path(const char *path, char *normalized);
bool fs_starts_with(const char *str, const char *prefix);

// Root volume sync helpers
void fat32_set_root_volume(void *fs_private);

// Desktop Limit
void fat32_set_desktop_limit(int limit);

#endif
