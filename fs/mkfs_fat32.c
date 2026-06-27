// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "mkfs_fat32.h"
#include "disk.h"
#include <stddef.h>
#include "memory_manager.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint64_t num);

// Internal helpers

static void mf_memset(void *dst, int val, int len) {
    unsigned char *p = (unsigned char *)dst;
    while (len-- > 0) *p++ = (unsigned char)val;
}

static void mf_memcpy(void *dst, const void *src, int len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (len-- > 0) *d++ = *s++;
}

static void mf_strncpy(char *dst, const char *src, int n) {
    int i = 0;
    while (i < n && src[i]) { dst[i] = src[i]; i++; }
    while (i < n) { dst[i++] = ' '; }  /* FAT labels are space-padded */
}

static void mf_set_disk_label(Disk *disk, const char *label) {
    int end = 11;
    while (end > 0 && label[end - 1] == ' ') end--;
    for (int i = 0; i < end && i < 31; i++) disk->label[i] = label[i];
    disk->label[end < 31 ? end : 31] = 0;
}

// On-disk BPB structures

typedef struct __attribute__((packed)) {
    /* DOS 2.0 BPB */
    uint8_t  jump_boot[3];          /* EB 58 90 */
    char     oem_name[8];           /* "MSDOS5.0" */
    uint16_t bytes_per_sector;      /* 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;              /* 2 */
    uint16_t root_entry_count;      /* 0 for FAT32 */
    uint16_t total_sectors_16;      /* 0 for FAT32 */
    uint8_t  media;                 /* 0xF8 for fixed disk */
    uint16_t fat_size_16;           /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;            /* 0x0000 */
    uint32_t root_cluster;          /* 2 */
    uint16_t fs_info;               /* sector 1 */
    uint16_t backup_boot_sector;    /* sector 6 */
    uint8_t  reserved[12];
    uint8_t  drive_number;          /* 0x80 for fixed disk */
    uint8_t  reserved1;
    uint8_t  boot_sig;              /* 0x29 */
    uint32_t volume_id;
    char     volume_label[11];      /* space-padded */
    char     fs_type[8];            /* "FAT32   " */
    uint8_t  boot_code[420];
    uint16_t boot_signature;        /* 0xAA55 */
} FAT32_BPB;

typedef struct __attribute__((packed)) {
    uint32_t lead_sig;              /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_sig;            /* 0x61417272 */
    uint32_t free_count;            /* 0xFFFFFFFF = unknown */
    uint32_t next_free;             /* 0xFFFFFFFF = unknown */
    uint8_t  reserved2[12];
    uint32_t trail_sig;             /* 0xAA550000 */
} FAT32_FSInfo;

// Public API

int mkfs_fat32_format(Disk *disk, uint32_t sector_count, const char *label) {
    if (sector_count < MIN_FAT32_SECTORS) {
        serial_write("[MKFS] Error: partition too small for FAT32 (< 32 MB)\n");
        return -1;
    }
    if (!disk || !disk->write_sector) {
        serial_write("[MKFS] Error: null disk or no write function\n");
        return -1;
    }

    uint8_t spc;  /* sectors per cluster */
    if      (sector_count <    532480) spc =  1;  /* < 260 MB  -> 512 B clusters */
    else if (sector_count <   1064960) spc =  2;  /* < 520 MB  -> 1 KB */
    else if (sector_count <   2097152) spc =  4;  /* < 1 GB    -> 2 KB */
    else if (sector_count <  16777216) spc =  8;  /* < 8 GB    -> 4 KB */
    else if (sector_count <  33554432) spc = 16;  /* < 16 GB   -> 8 KB */
    else if (sector_count <  67108864) spc = 32;  /* < 32 GB   -> 16 KB */
    else                               spc = 64;  /* >= 32 GB  -> 32 KB */

    const uint32_t reserved_sectors = 32;
    const uint8_t  num_fats = 2;
    const uint32_t root_cluster = 2;

    uint32_t data_sectors = sector_count - reserved_sectors;
    uint32_t cluster_count = data_sectors / spc;
    uint32_t fat_bytes = (cluster_count + 2) * 4;
    uint32_t sectors_per_fat = (fat_bytes + 511) / 512;

    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) {
        serial_write("[MKFS] Error: out of memory\n");
        return -1;
    }

    FAT32_BPB *bpb = (FAT32_BPB *)buf;
    mf_memset(bpb, 0, 512);

    bpb->jump_boot[0] = 0xEB;
    bpb->jump_boot[1] = 0x58;
    bpb->jump_boot[2] = 0x90;
    mf_memcpy(bpb->oem_name, "MSDOS5.0", 8);
    bpb->bytes_per_sector    = 512;
    bpb->sectors_per_cluster = spc;
    bpb->reserved_sector_count = (uint16_t)reserved_sectors;
    bpb->num_fats            = num_fats;
    bpb->root_entry_count    = 0;
    bpb->total_sectors_16    = 0;
    bpb->media               = 0xF8;
    bpb->fat_size_16         = 0;
    bpb->sectors_per_track   = 63;
    bpb->num_heads           = 255;
    bpb->hidden_sectors      = disk->partition_lba_offset;
    bpb->total_sectors_32    = sector_count;
    bpb->fat_size_32         = sectors_per_fat;
    bpb->ext_flags           = 0;
    bpb->fs_version          = 0;
    bpb->root_cluster        = root_cluster;
    bpb->fs_info             = 1;
    bpb->backup_boot_sector  = 6;
    bpb->drive_number        = 0x80;
    bpb->boot_sig            = 0x29;
    bpb->volume_id           = 0x12345678; /* arbitrary non-zero volume ID */

    /* Volume label */
    const char *vol_label = (label && label[0]) ? label : "NO NAME    ";
    char upper_label[11];
    for (int i = 0; i < 11; i++) {
        if (vol_label[i] == 0) {
            for (int j = i; j < 11; j++) upper_label[j] = ' ';
            break;
        }
        char c = vol_label[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        upper_label[i] = c;
    }
    mf_memcpy(bpb->volume_label, upper_label, 11);
    mf_memcpy(bpb->fs_type, "FAT32   ", 8);

    /* Boot sector signature */
    bpb->boot_signature = 0xAA55;

    /* Write sector 0 (BPB) */
    if (disk->write_sector(disk, 0, buf) != 0) {
        serial_write("[MKFS] Error: failed to write BPB (sector 0)\n");
        kfree(buf);
        return -1;
    }

    FAT32_FSInfo *fsinfo = (FAT32_FSInfo *)buf;
    mf_memset(fsinfo, 0, 512);
    fsinfo->lead_sig   = 0x41615252;
    fsinfo->struct_sig = 0x61417272;
    fsinfo->free_count = 0xFFFFFFFF; 
    fsinfo->next_free  = 0xFFFFFFFF; 
    fsinfo->trail_sig  = 0xAA550000;

    if (disk->write_sector(disk, 1, buf) != 0) {
        serial_write("[MKFS] Error: failed to write FSInfo (sector 1)\n");
        kfree(buf);
        return -1;
    }
    FAT32_BPB *bpb2 = (FAT32_BPB *)buf;
    mf_memset(bpb2, 0, 512);
    mf_memcpy(bpb2->jump_boot, "\xEB\x58\x90", 3);
    mf_memcpy(bpb2->oem_name, "MSDOS5.0", 8);
    bpb2->bytes_per_sector    = 512;
    bpb2->sectors_per_cluster = spc;
    bpb2->reserved_sector_count = (uint16_t)reserved_sectors;
    bpb2->num_fats            = num_fats;
    bpb2->root_entry_count    = 0;
    bpb2->total_sectors_16    = 0;
    bpb2->media               = 0xF8;
    bpb2->fat_size_16         = 0;
    bpb2->sectors_per_track   = 63;
    bpb2->num_heads           = 255;
    bpb2->hidden_sectors      = disk->partition_lba_offset;
    bpb2->total_sectors_32    = sector_count;
    bpb2->fat_size_32         = sectors_per_fat;
    bpb2->ext_flags           = 0;
    bpb2->fs_version          = 0;
    bpb2->root_cluster        = root_cluster;
    bpb2->fs_info             = 1;
    bpb2->backup_boot_sector  = 6;
    bpb2->drive_number        = 0x80;
    bpb2->boot_sig            = 0x29;
    bpb2->volume_id           = 0x12345678;
    mf_memcpy(bpb2->volume_label, upper_label, 11);
    mf_memcpy(bpb2->fs_type, "FAT32   ", 8);
    bpb2->boot_signature = 0xAA55;

    if (disk->write_sector(disk, 6, buf) != 0) {
        serial_write("[MKFS] Error: failed to write backup BPB (sector 6)\n");
        kfree(buf);
        return -1;
    }

    FAT32_FSInfo *fsinfo2 = (FAT32_FSInfo *)buf;
    mf_memset(fsinfo2, 0, 512);
    fsinfo2->lead_sig   = 0x41615252;
    fsinfo2->struct_sig = 0x61417272;
    fsinfo2->free_count = 0xFFFFFFFF;
    fsinfo2->next_free  = 0xFFFFFFFF;
    fsinfo2->trail_sig  = 0xAA550000;

    if (disk->write_sector(disk, 7, buf) != 0) {
        serial_write("[MKFS] Error: failed to write backup FSInfo (sector 7)\n");
        kfree(buf);
        return -1;
    }

    /* Zero both FATs */
    mf_memset(buf, 0, 512);
    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = reserved_sectors + (f * sectors_per_fat);
        for (uint32_t s = 0; s < sectors_per_fat; s++) {
            if (disk->write_sector(disk, fat_start + s, buf) != 0) {
                serial_write("[MKFS] Error: failed to zero FAT\n");
                kfree(buf);
                return -1;
            }
        }
    }

    /* Write markers to both FATs */
    mf_memset(buf, 0, 512);
    uint32_t *fat_buf = (uint32_t *)buf;
    fat_buf[0] = 0x0FFFFFF8; // Media type
    fat_buf[1] = 0x0FFFFFFF; // Reserved
    fat_buf[2] = 0x0FFFFFFF; // Root directory (Cluster 2)

    for (uint32_t f = 0; f < num_fats; f++) {
        uint32_t fat_start = reserved_sectors + (f * sectors_per_fat);
        if (disk->write_sector(disk, fat_start, buf) != 0) {
            serial_write("[MKFS] Error: failed to write FAT markers\n");
            kfree(buf);
            return -1;
        }
    }

    /* Zero root cluster */
    mf_memset(buf, 0, 512);
    uint32_t root_start = reserved_sectors + num_fats * sectors_per_fat;
    for (uint32_t s = 0; s < (uint32_t)spc; s++) {
        if (disk->write_sector(disk, root_start + s, buf) != 0) {
            serial_write("[MKFS] Error: failed to zero root cluster\n");
            kfree(buf);
            return -1;
        }
    }

    kfree(buf);

    disk->is_fat32 = true;
    mf_set_disk_label(disk, upper_label);

    serial_write("[MKFS] FAT32 formatted: ");
    serial_write(disk->devname);
    serial_write(" label=");
    char lb[12];
    mf_memcpy(lb, upper_label, 11);
    lb[11] = 0;
    for (int i = 10; i >= 0 && lb[i] == ' '; i--) lb[i] = 0;
    serial_write(lb);
    serial_write(" spc=");
    serial_write_num(spc);
    serial_write(" fat_sectors=");
    serial_write_num(sectors_per_fat);
    serial_write("\n");

    return 0;
}
