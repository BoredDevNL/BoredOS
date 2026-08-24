// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "disk.h"
#include "pci.h"
#include "slab.h"
#include "io.h"
#include "ahci.h"
#include "vfs.h"
#include "fat32.h"
#include "ext4fs.h"
#include "spinlock.h"
#include <stddef.h>
#include "kutils.h"

static spinlock_t ide_lock = SPINLOCK_INIT;

static Disk *disks[MAX_DISKS];
static int disk_count = 0;
static int next_drive_letter_idx = 0;  // For backward compat
static int next_sd_index = 0;  // For sda, sdb, sdc...
static int next_hd_index = 0;  // For hda, hdb, hdc...

extern void serial_write(const char *str);
extern void serial_write_num(uint64_t num);
extern void log_ok(const char *msg);
extern void log_fail(const char *msg);

// === String Helpers ===

static void dm_copy_fat_label(char *dst, const uint8_t *src) {
    int end = 11;
    while (end > 0 && src[end - 1] == ' ') end--;
    for (int i = 0; i < end && i < 31; i++) dst[i] = (char)src[i];
    dst[end < 31 ? end : 31] = 0;
}

static void disk_load_fat32_label(Disk *disk) {
    uint8_t *buffer;
    FAT32_BootSector *bpb;
    char label[32];

    if (!disk || !disk->read_sector) return;

    buffer = (uint8_t*)kmalloc(512);
    if (!buffer) return;

    if (disk->read_sector(disk, 0, buffer) == 0 && buffer[510] == 0x55 && buffer[511] == 0xAA) {
        bpb = (FAT32_BootSector*)buffer;
        dm_copy_fat_label(label, bpb->volume_label);
        if (label[0]) strcpy(disk->label, label);
    }

    kfree_null(buffer);
}

// === ATA Definitions (Legacy IDE PIO — kept as fallback) ===

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SEC_COUNT0 0x02
#define ATA_REG_LBA0       0x03
#define ATA_REG_LBA1       0x04
#define ATA_REG_LBA2       0x05
#define ATA_REG_HDDEVSEL   0x06
#define ATA_REG_COMMAND    0x07
#define ATA_REG_STATUS     0x07

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC

#define ATA_SR_BSY     0x80
#define ATA_SR_DRDY    0x40
#define ATA_SR_DF      0x20
#define ATA_SR_DSC     0x10
#define ATA_SR_DRQ     0x08
#define ATA_SR_CORR    0x04
#define ATA_SR_IDX     0x02
#define ATA_SR_ERR     0x01

typedef struct {
    uint16_t port_base;
    bool slave;
} ATADriverData;

// === ATA PIO Driver ===

static int ata_wait_bsy(uint16_t port_base) {
    int timeout = 10000000;
    while ((inb(port_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout > 0);
    return timeout <= 0 ? -1 : 0;
}

static int ata_wait_drq(uint16_t port_base) {
    int timeout = 10000000;
    while (!(inb(port_base + ATA_REG_STATUS) & (ATA_SR_DRQ | ATA_SR_ERR)) && --timeout > 0);
    if (timeout <= 0 || (inb(port_base + ATA_REG_STATUS) & ATA_SR_ERR)) return -1;
    return 0;
}

static int ata_identify(uint16_t port_base, bool slave) {
    outb(port_base + ATA_REG_HDDEVSEL, slave ? 0xB0 : 0xA0);
    outb(port_base + ATA_REG_SEC_COUNT0, 0);
    outb(port_base + ATA_REG_LBA0, 0);
    outb(port_base + ATA_REG_LBA1, 0);
    outb(port_base + ATA_REG_LBA2, 0);

    outb(port_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(port_base + ATA_REG_STATUS);
    if (status == 0) return 0;

    int timeout = 10000;
    while ((inb(port_base + ATA_REG_STATUS) & ATA_SR_BSY) && --timeout > 0) {
        status = inb(port_base + ATA_REG_STATUS);
        if (status == 0) return 0;
    }
    if (timeout <= 0) return 0;

    if (inb(port_base + ATA_REG_STATUS) & ATA_SR_ERR) return 0;

    if (ata_wait_drq(port_base) != 0) return 0;

    if (inb(port_base + ATA_REG_STATUS) & ATA_SR_ERR) return 0;

    uint32_t sectors = 0;
    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(port_base + ATA_REG_DATA);
        if (i == 60) sectors |= (uint32_t)data;
        if (i == 61) sectors |= (uint32_t)data << 16;
    }

    return sectors;
}

static void ata_resolve_partition(Disk *disk, uint32_t *lba,
                                  uint16_t *port_base, bool *slave) {
    ATADriverData *data = (ATADriverData*)disk->driver_data;
    *port_base = data->port_base;
    *slave = data->slave;
    if (disk->is_partition && disk->parent) {
        *lba += disk->partition_lba_offset;
        data = (ATADriverData*)disk->parent->driver_data;
        *port_base = data->port_base;
        *slave = data->slave;
    }
}

static int ata_read_sector(Disk *disk, uint32_t lba, uint8_t *buffer) {
    uint16_t port_base;
    bool slave;
    ata_resolve_partition(disk, &lba, &port_base, &slave);

    uint64_t flags = spinlock_acquire_irqsave(&ide_lock);

    if (ata_wait_bsy(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }

    outb(port_base + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
    outb(port_base + ATA_REG_FEATURES, 0x00);
    outb(port_base + ATA_REG_SEC_COUNT0, 1);
    outb(port_base + ATA_REG_LBA0, (uint8_t)(lba));
    outb(port_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(port_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(port_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_bsy(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }
    if (ata_wait_drq(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }

    uint16_t *ptr = (uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(port_base + ATA_REG_DATA);
    }

    spinlock_release_irqrestore(&ide_lock, flags);
    return 0;
}

static int ata_write_sector(Disk *disk, uint32_t lba, const uint8_t *buffer) {
    uint16_t port_base;
    bool slave;
    ata_resolve_partition(disk, &lba, &port_base, &slave);

    uint64_t flags = spinlock_acquire_irqsave(&ide_lock);

    if (ata_wait_bsy(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }

    outb(port_base + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
    outb(port_base + ATA_REG_FEATURES, 0x00);
    outb(port_base + ATA_REG_SEC_COUNT0, 1);
    outb(port_base + ATA_REG_LBA0, (uint8_t)(lba));
    outb(port_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(port_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(port_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (ata_wait_bsy(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }
    if (ata_wait_drq(port_base) != 0) {
        spinlock_release_irqrestore(&ide_lock, flags);
        return -1;
    }

    const uint16_t *ptr = (const uint16_t*)buffer;
    for (int i = 0; i < 256; i++) {
        outw(port_base + ATA_REG_DATA, ptr[i]);
    }

    spinlock_release_irqrestore(&ide_lock, flags);
    return 0;
}

static int ata_read_sectors(Disk *disk, uint32_t lba, uint32_t count, uint8_t *buffer) {
    uint16_t port_base;
    bool slave;
    ata_resolve_partition(disk, &lba, &port_base, &slave);

    uint64_t flags = spinlock_acquire_irqsave(&ide_lock);

    while (count > 0) {
        uint8_t batch = (count > 255) ? 255 : (uint8_t)count;
        if (ata_wait_bsy(port_base) != 0) {
            spinlock_release_irqrestore(&ide_lock, flags);
            return -1;
        }

        outb(port_base + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
        outb(port_base + ATA_REG_SEC_COUNT0, batch);
        outb(port_base + ATA_REG_LBA0, (uint8_t)(lba));
        outb(port_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(port_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(port_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

        for (uint8_t b = 0; b < batch; b++) {
            if (ata_wait_bsy(port_base) != 0 || ata_wait_drq(port_base) != 0) {
                spinlock_release_irqrestore(&ide_lock, flags);
                return -1;
            }
            uint16_t *pptr = (uint16_t*)(buffer + (b * 512));
            for (int i = 0; i < 256; i++) {
                pptr[i] = inw(port_base + ATA_REG_DATA);
            }
        }
        lba += batch;
        buffer += batch * 512;
        count -= batch;
    }

    spinlock_release_irqrestore(&ide_lock, flags);
    return 0;
}

static int ata_write_sectors(Disk *disk, uint32_t lba, uint32_t count, const uint8_t *buffer) {
    uint16_t port_base;
    bool slave;
    ata_resolve_partition(disk, &lba, &port_base, &slave);

    uint64_t flags = spinlock_acquire_irqsave(&ide_lock);

    while (count > 0) {
        uint8_t batch = (count > 255) ? 255 : (uint8_t)count;
        if (ata_wait_bsy(port_base) != 0) {
            spinlock_release_irqrestore(&ide_lock, flags);
            return -1;
        }

        outb(port_base + ATA_REG_HDDEVSEL, 0xE0 | (slave << 4) | ((lba >> 24) & 0x0F));
        outb(port_base + ATA_REG_SEC_COUNT0, batch);
        outb(port_base + ATA_REG_LBA0, (uint8_t)(lba));
        outb(port_base + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(port_base + ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(port_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

        for (uint8_t b = 0; b < batch; b++) {
            if (ata_wait_bsy(port_base) != 0 || ata_wait_drq(port_base) != 0) {
                spinlock_release_irqrestore(&ide_lock, flags);
                return -1;
            }
            const uint16_t *pptr = (const uint16_t*)(buffer + (b * 512));
            for (int i = 0; i < 256; i++) {
                outw(port_base + ATA_REG_DATA, pptr[i]);
            }
        }
        
        lba += batch;
        buffer += batch * 512;
        count -= batch;
    }

    spinlock_release_irqrestore(&ide_lock, flags);
    return 0;
}

// === Device Naming ===

const char* disk_get_next_dev_name(DiskType type) {
    static char name[8];
    if (type == DISK_TYPE_IDE) {
        name[0] = 'h';
        name[1] = 'd';
        name[2] = 'a' + next_hd_index;
        name[3] = 0;
        next_hd_index++;
    } else {
        name[0] = 's';
        name[1] = 'd';
        name[2] = 'a' + next_sd_index;
        name[3] = 0;
        next_sd_index++;
    }
    return name;
}

// === Registration ===

void disk_register(Disk *disk) {
    if (disk_count >= MAX_DISKS) return;

    // Auto-assign devname if empty
    if (disk->devname[0] == 0) {
        const char *n = disk_get_next_dev_name(disk->type);
        strcpy(disk->devname, n);
    }

    disk->registered = true;
    disks[disk_count++] = disk;

    serial_write("[DISK] Registered /dev/");
    serial_write(disk->devname);
    serial_write(" (");
    serial_write(disk->label);
    serial_write(") size=");
    serial_write_num(disk->total_sectors);
    serial_write(" sectors\n");
}

void disk_register_partition(Disk *parent, uint32_t lba_offset, uint32_t sector_count,
                             bool is_fat32, bool is_esp, int part_num) {
    if (disk_count >= MAX_DISKS) return;

    Disk *part = (Disk*)kmalloc(sizeof(Disk));
    if (!part) return;
    memset(part, 0, sizeof(Disk));

    // Build name: parent_devname + partition number (e.g. "sda1")
    size_t len = strlen(parent->devname);
    for (size_t i = 0; i < len; i++) part->devname[i] = parent->devname[i];
    part->devname[len] = '0' + part_num;
    part->devname[len + 1] = 0;

    part->type = parent->type;
    part->is_fat32 = is_fat32;
    part->is_esp = is_esp;
    strcpy(part->label, is_esp ? "EFI System Partition" : (is_fat32 ? "FAT32 Partition" : "Unknown Partition"));
    part->partition_lba_offset = lba_offset;
    part->total_sectors = sector_count;
    part->read_sector = parent->read_sector;
    part->write_sector = parent->write_sector;
    part->read_sectors = parent->read_sectors;
    part->write_sectors = parent->write_sectors;
    part->sync = parent->sync;
    part->driver_data = parent->driver_data;
    part->parent = parent;
    part->is_partition = true;
    part->registered = true;

    if (is_fat32) disk_load_fat32_label(part);

    disks[disk_count++] = part;

    serial_write("[DISK] Registered /dev/");
    serial_write(part->devname);
    serial_write(" (LBA offset ");
    serial_write_num(lba_offset);
    serial_write(", ");
    serial_write_num(sector_count);
    serial_write(" sectors, FAT32=");
    serial_write(is_fat32 ? "yes" : "no");
    if (is_esp) serial_write(", ESP=yes");
    serial_write(")\n");

}

// === Lookup ===

Disk* disk_get_by_name(const char *devname) {
    if (!devname) return NULL;
    if (strncmp(devname, "/dev/", 5) == 0) devname += 5;
    for (int i = 0; i < disk_count; i++) {
        if (strcmp(disks[i]->devname, devname) == 0) {
            return disks[i];
        }
    }
    return NULL;
}

int disk_get_count(void) {
    return disk_count;
}

Disk* disk_get_by_index(int index) {
    if (index < 0 || index >= disk_count) return NULL;
    return disks[index];
}

// === MBR Partition Table ===

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) MBR_PartitionEntry;

#define PART_TYPE_FAT32     0x0B
#define PART_TYPE_FAT32_LBA 0x0C

static bool is_fat32_bpb(const uint8_t *sector) {
    if (sector[510] != 0x55 || sector[511] != 0xAA) return false;

    if (sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' &&
        sector[85] == '3' && sector[86] == '2') {
        return true;
    }

    uint16_t bps = *(uint16_t*)&sector[11];
    uint16_t spf16 = *(uint16_t*)&sector[22];
    uint32_t spf32 = *(uint32_t*)&sector[36];
    if (bps == 512 && spf16 == 0 && spf32 > 0) {
        return true;
    }

    return false;
}

static bool disk_probe_ext4(Disk *disk, uint32_t lba) {
    uint8_t *pbuf = (uint8_t*)kmalloc(512);
    if (!pbuf) return false;
    bool ext4 = false;
    if (disk->read_sector(disk, lba + 2, pbuf) == 0) {
        uint16_t magic = *(uint16_t*)(pbuf + 56);
        if (magic == EXT4_SUPERBLOCK_MAGIC) ext4 = true;
    }
    kfree_null(pbuf);
    return ext4;
}

static bool disk_probe_fat32(Disk *disk, uint32_t lba) {
    uint8_t *pbuf = (uint8_t*)kmalloc(512);
    if (!pbuf) return false;
    bool fat32 = false;
    if (disk->read_sector(disk, lba, pbuf) == 0)
        fat32 = is_fat32_bpb(pbuf);
    kfree_null(pbuf);
    return fat32;
}

// Parse MBR and register each partition as a child block device
static void parse_mbr_partitions(Disk *disk) {
    uint8_t *buffer = (uint8_t*)kmalloc(512);
    if (!buffer) return;

    if (disk->read_sector(disk, 0, buffer) != 0) {
        serial_write("[DISK] MBR read failed on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
        kfree_null(buffer);
        return;
    }

    // Check for valid MBR signature
    if (buffer[510] != 0x55 || buffer[511] != 0xAA) {
        serial_write("[DISK] Invalid MBR signature on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
        kfree_null(buffer);
        return;
    }

    MBR_PartitionEntry *partitions = (MBR_PartitionEntry*)&buffer[446];
    int part_num = 1;

    int part_count = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t start = partitions[i].lba_start;
        uint32_t size = partitions[i].sector_count;
        uint8_t type = partitions[i].type;

        if (type == 0x00) continue; // Empty entry
        if (size == 0) continue;
        if (start >= disk->total_sectors) continue; // Invalid start
        
        bool fat32 = false;
        if (type == PART_TYPE_FAT32 || type == PART_TYPE_FAT32_LBA) {
            fat32 = disk_probe_fat32(disk, start);
        }

        disk_register_partition(disk, partitions[i].lba_start,
                                partitions[i].sector_count, fat32, false, part_num);
        part_count++;
        part_num++;
    }

    // Fallback: if no partitions found, check if entire disk is a raw FAT32 volume
    if (part_num == 1 && is_fat32_bpb(buffer)) {
        serial_write("[DISK] No MBR partitions — raw FAT32 volume on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
        disk->is_fat32 = true;
        disk->partition_lba_offset = 0;
        disk_load_fat32_label(disk);
    } else if (part_count == 0) {
        serial_write("[DISK] No MBR partitions found on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
    }

    kfree_null(buffer);
}

// === ATA Drive Discovery ===

static void try_add_ata_drive(uint16_t port, bool slave, const char *name) {
    uint32_t sectors = ata_identify(port, slave);
    if (sectors > 0) {
        Disk *new_disk = (Disk*)kmalloc(sizeof(Disk));
        if (!new_disk) return;
        memset(new_disk, 0, sizeof(Disk));

        ATADriverData *data = (ATADriverData*)kmalloc(sizeof(ATADriverData));
        data->port_base = port;
        data->slave = slave;

        new_disk->devname[0] = 0; // Auto-assign
        new_disk->type = DISK_TYPE_IDE;
        strcpy(new_disk->label, name);
        new_disk->read_sector = ata_read_sector;
        new_disk->write_sector = ata_write_sector;
        new_disk->read_sectors = ata_read_sectors;
        new_disk->write_sectors = ata_write_sectors;
        new_disk->driver_data = data;
        new_disk->partition_lba_offset = 0;
        new_disk->total_sectors = sectors;
        new_disk->parent = NULL;
        new_disk->is_partition = false;
        new_disk->is_fat32 = false;

        disk_register(new_disk);

        // Parse MBR to find partitions
        parse_mbr_partitions(new_disk);
    }
}

// === Init & Scan ===

void disk_manager_init(void) {
    for (int i = 0; i < MAX_DISKS; i++) {
        disks[i] = NULL;
    }
    disk_count = 0;
    next_sd_index = 0;
    next_hd_index = 0;
    next_drive_letter_idx = 0;

    log_ok("Disk manager ready");
}

void disk_manager_scan(void) {
    serial_write("[DISK] Initializing AHCI (SATA DMA)...\n");
    ahci_init();
    
    if (ahci_get_port_count() == 0) {
        serial_write("[DISK] No AHCI ports found, falling back to legacy IDE...\n");
        try_add_ata_drive(ATA_PRIMARY_IO, false, "IDE Primary Master");
        try_add_ata_drive(ATA_PRIMARY_IO, true, "IDE Primary Slave");
        try_add_ata_drive(ATA_SECONDARY_IO, false, "IDE Secondary Master");
        try_add_ata_drive(ATA_SECONDARY_IO, true, "IDE Secondary Slave");
        log_ok("IDE probing complete");
    } else {
        log_ok("AHCI ports initialized, skipping IDE");
    }
}


static uint32_t crc32_compute(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}


#define GPT_PART_ENTRY_COUNT 128
#define GPT_PART_ENTRY_SIZE  128
_Static_assert(GPT_PART_ENTRY_COUNT * GPT_PART_ENTRY_SIZE == 32 * 512,
               "GPT partition array must be exactly 32 sectors");

static const uint8_t GPT_GUID_ESP[16]        = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
static const uint8_t GPT_GUID_BASIC_DATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

typedef struct __attribute__((packed)) {
    uint64_t signature;         
    uint32_t revision;          
    uint32_t header_size;       
    uint32_t crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t size_of_partition_entry;
    uint32_t partition_entry_array_crc32;
} GPT_Header;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  partition_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name[36];  
} GPT_Entry;

static void gpt_make_pseudo_guid(uint8_t *guid, const char *label, uint32_t total_sectors) {
    uint32_t h = 5381;
    for (int i = 0; label[i]; i++)
        h = h * 33 + (unsigned char)label[i];
    h ^= total_sectors;
    for (int i = 0; i < 16; i++)
        guid[i] = (uint8_t)(h >> ((i % 4) * 8));
    guid[8] = (guid[8] & 0x3F) | 0x80;
    guid[6] = (guid[6] & 0x0F) | 0x40;
}

int disk_sync(Disk *disk) {
    if (!disk) return -1;
    Disk *target = disk->parent ? disk->parent : disk;

    // Use device-specific sync if available
    if (target->sync) return target->sync(target);

    if (target->type == DISK_TYPE_IDE) {
        ATADriverData *data = (ATADriverData *)target->driver_data;
        if (!data) return -1;
        uint64_t flags = spinlock_acquire_irqsave(&ide_lock);
        if (ata_wait_bsy(data->port_base) == 0) {
            outb(data->port_base + ATA_REG_HDDEVSEL, data->slave ? 0xB0 : 0xA0);
            outb(data->port_base + ATA_REG_COMMAND, 0xE7); 
            ata_wait_bsy(data->port_base);
        }
        spinlock_release_irqrestore(&ide_lock, flags);
        return 0;
    }
    return 0;
}


static void disk_remove_partitions(Disk *parent) {
    for (int i = 0; i < disk_count; i++) {
        if (disks[i] && disks[i]->parent == parent) {
            Disk *p = disks[i];
            
            // Unmount from VFS if it's mounted
            char mount_path[32];
            mount_path[0] = '/';
            mount_path[1] = 'd'; mount_path[2] = 'e'; mount_path[3] = 'v'; mount_path[4] = '/';
            strcpy(mount_path + 5, p->devname);
            
            extern bool vfs_umount(const char *mount_path);
            vfs_umount(mount_path);

            for (int j = i; j < disk_count - 1; j++) {
                disks[j] = disks[j + 1];
            }
            disks[disk_count - 1] = NULL;
            disk_count--;
            i--;
            kfree_null(p);
        }
    }
}

static void parse_gpt_partitions(Disk *disk) {
    uint8_t *buffer = (uint8_t*)kmalloc(512);
    if (!buffer) return;

    if (disk->read_sector(disk, 1, buffer) != 0) {
        serial_write("[DISK] GPT header read failed on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
        kfree_null(buffer);
        return;
    }

    GPT_Header *hdr = (GPT_Header *)buffer;
    if (hdr->signature != 0x5452415020494645ULL) {
        serial_write("[DISK] GPT signature missing on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
        kfree_null(buffer);
        return;
    }

    uint32_t num_entries = hdr->num_partition_entries;
    uint32_t entry_size = hdr->size_of_partition_entry;
    uint64_t entry_lba = hdr->partition_entry_lba;

    uint8_t *entry_buf = (uint8_t*)kmalloc(512);
    if (!entry_buf) { kfree_null(buffer); return; }

    int part_num = 1;
    int part_count = 0;
    for (uint32_t i = 0; i < num_entries && i < 128; i++) {
        uint32_t entry_lba_offset = (uint32_t)entry_lba + (i * entry_size) / 512;
        uint32_t entry_sector_offset = (i * entry_size) % 512;

        if (disk->read_sector(disk, entry_lba_offset, entry_buf) != 0) break;

        GPT_Entry *entry = (GPT_Entry *)(entry_buf + entry_sector_offset);

        bool zero = true;
        for (int j = 0; j < 16; j++) if (entry->type_guid[j] != 0) { zero = false; break; }
        if (zero) continue;

        uint32_t start = (uint32_t)entry->start_lba;
        uint32_t end   = (uint32_t)entry->end_lba;
        uint32_t size  = end - start + 1;

        if (size == 0) continue;

        static const uint8_t esp_guid[16] = {
            0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 
            0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
        };
        bool is_esp = true;
        for (int j = 0; j < 16; j++) if (entry->type_guid[j] != esp_guid[j]) { is_esp = false; break; }

        bool is_ext4 = disk_probe_ext4(disk, start);
        bool fat32 = !is_ext4 && (is_esp || disk_probe_fat32(disk, start));

        disk_register_partition(disk, start, size, fat32, is_esp, part_num++);
        part_count++;
    }

    if (part_count == 0) {
        serial_write("[DISK] GPT found but no partitions registered on /dev/");
        serial_write(disk->devname);
        serial_write("\n");
    }

    kfree_null(entry_buf);
    kfree_null(buffer);
}

int disk_rescan(Disk *disk) {
    if (!disk || disk->is_partition) return -1;
    
    disk_remove_partitions(disk);

    serial_write("[DISK] Rescanning /dev/");
    serial_write(disk->devname);
    serial_write("\n");

    uint8_t *buffer = (uint8_t*)kmalloc(512);
    if (buffer) {
        if (disk->read_sector(disk, 1, buffer) == 0) {
            GPT_Header *hdr = (GPT_Header*)buffer;
            if (hdr->signature == 0x5452415020494645ULL) {
                serial_write("[DISK] GPT detected on /dev/");
                serial_write(disk->devname);
                serial_write("\n");
                kfree_null(buffer);
                parse_gpt_partitions(disk);
                return 0;
            }
        } else {
            serial_write("[DISK] GPT probe read failed on /dev/");
            serial_write(disk->devname);
            serial_write("\n");
        }
        kfree_null(buffer);
    }

    parse_mbr_partitions(disk);
    return 0;
}
