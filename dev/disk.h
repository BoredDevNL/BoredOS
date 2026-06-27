// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef DISK_H
#define DISK_H

#include <stdint.h>
#include <stdbool.h>

#define SECTOR_SIZE 512
#define MAX_DISKS 16

typedef enum {
    DISK_TYPE_RAM,
    DISK_TYPE_IDE,
    DISK_TYPE_SATA,
    DISK_TYPE_USB
} DiskType;

typedef struct Disk {
    char devname[16];
    DiskType type;
    bool is_fat32;
    bool is_esp;
    char label[32];
    uint32_t partition_lba_offset;
    uint32_t total_sectors;
    int (*read_sector)(struct Disk *disk, uint32_t sector, uint8_t *buffer);
    int (*write_sector)(struct Disk *disk, uint32_t sector, const uint8_t *buffer);
    int (*read_sectors)(struct Disk *disk, uint32_t sector, uint32_t count, uint8_t *buffer);
    int (*write_sectors)(struct Disk *disk, uint32_t sector, uint32_t count, const uint8_t *buffer);
    int (*sync)(struct Disk *disk);

    // Private driver data
    void *driver_data;

    // Parent disk (for partitions — points to the whole-disk Disk)
    struct Disk *parent;
    bool is_partition;
    bool registered;
} Disk;

typedef struct {
    uint32_t lba_start;
    uint32_t sector_count;
    uint8_t  part_type;
    uint8_t  flags;
    char     label[36];
} disk_partition_spec_t;

#define PART_FLAG_ESP       0x01
#define MIN_INSTALL_SECTORS 2097152 

// Initialization and scanning
void disk_manager_init(void);
void disk_manager_scan(void);

// Device registration
void disk_register(Disk *disk);
void disk_register_partition(Disk *parent, uint32_t lba_offset, uint32_t sector_count,
                             bool is_fat32, bool is_esp, int part_num);

// Lookup
Disk* disk_get_by_name(const char *devname);
int disk_get_count(void);
Disk* disk_get_by_index(int index);

// Auto-naming helpers
const char* disk_get_next_dev_name(DiskType type);   // Returns "sda"/"hda", etc.

// Backward compat (deprecated — wraps disk_get_by_name)
Disk* disk_get_by_letter(char letter);
char disk_get_next_free_letter(void);

int disk_write_gpt(Disk *disk, disk_partition_spec_t *parts, int count);
int disk_write_mbr(Disk *disk, disk_partition_spec_t *parts, int count);

int disk_sync(Disk *disk);

int disk_rescan(Disk *disk);

#endif
