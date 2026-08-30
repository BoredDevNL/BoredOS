// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "fat32.h"
#include "vfs.h"
#include "slab.h"
#include "io.h"
#include "disk.h"
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "spinlock.h"
#include "kutils.h"
#include <string.h>

typedef struct fat32_volume {
    Disk *disk;
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t fat_size;
    uint32_t num_fats;
    uint32_t total_sectors;
    uint32_t partition_offset;
    bool mounted;
    uint32_t cached_fat_sector;
    uint8_t cached_fat_buf[512];
    uint32_t last_allocated_cluster;
    spinlock_t lock;
} fat32_volume_t;

typedef fat32_volume_t FAT32_Volume;

#define MAX_FAT32_VOLUMES 8
static fat32_volume_t *fat32_volumes[MAX_FAT32_VOLUMES];
static int fat32_volume_count = 0;
static fat32_volume_t *root_volume = NULL;
static spinlock_t fat32_subsystem_lock = SPINLOCK_INIT;

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);
extern void serial_write_hex(uint64_t val);

static uint32_t fat32_allocate_cluster(fat32_volume_t *vol);
static void fat32_truncate(fat32_file_handle_t *handle);
static fat32_file_handle_t *fat32_open_vol(fat32_volume_t *vol, const char *path, const char *mode);
static int fat32_list_directory_vol(fat32_volume_t *vol, const char *path, fat32_file_info_t *entries, int max_entries, int offset);
static bool fat32_delete_vol(fat32_volume_t *vol, const char *path);
static bool fat32_mount_vol(fat32_volume_t *vol, Disk *disk);
static void fat32_update_dir_entry_size(fat32_volume_t *vol, fat32_file_handle_t *handle);
static int fat32_read_cluster(fat32_volume_t *vol, uint32_t cluster, uint8_t *buffer);
static int fat32_write_cluster(fat32_volume_t *vol, uint32_t cluster, const uint8_t *buffer);
static uint32_t fat32_next_cluster(fat32_volume_t *vol, uint32_t cluster);
static bool fat32_find_contiguous_free(fat32_volume_t *vol, uint32_t dir_start_cluster, int n, uint32_t *out_cluster, size_t *out_entry_idx);
static uint8_t fat_lfn_checksum(const uint8_t *short_name);
static void extract_lfn_chars(fat32_lfn_entry_t *lfn, char *buffer);
static void to_dos_filename(const char *filename, char *dos_name);
static bool fat32_create_entry(fat32_volume_t *vol, uint32_t parent_cluster, const char *name, uint8_t attributes, uint32_t start_cluster, uint32_t file_size, uint32_t *out_sector, uint32_t *out_offset);
static bool fat32_mkdir_vol(fat32_volume_t *vol, const char *path);

void fat32_set_root_volume(void *fs_private) {
    root_volume = (fat32_volume_t *)fs_private;
}

static void fat32_sync_if_root(fat32_volume_t *vol) {
    if (!vol || vol != root_volume) return;
    disk_sync(vol->disk);
}

static void fat32_resolve_entry_name(const fat32_dir_entry_t *entry,
                                     const char *lfn_buffer, bool has_lfn,
                                     char *name) {
    if (has_lfn && lfn_buffer[0] != 0) {
        strcpy(name, lfn_buffer);
    } else {
        int n = 0;
        for (int k = 0; k < 8 && entry->filename[k] != ' '; k++)
            name[n++] = entry->filename[k];
        if (entry->extension[0] != ' ') {
            name[n++] = '.';
            for (int k = 0; k < 3 && entry->extension[k] != ' '; k++)
                name[n++] = entry->extension[k];
        }
        name[n] = 0;
    }
}

static bool fat32_name_match(const char *a, const char *b) {
    int i = 0;
    for (; a[i] && b[i]; i++) {
        char c1 = a[i], c2 = b[i];
        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
        if (c1 != c2) return false;
    }
    return a[i] == b[i];
}

static void extract_filename(const char *path, char *filename) {
    size_t len = strlen(path);
    if (len == 0) {
        filename[0] = '\0';
        return;
    }

    size_t i = len;
    while (i > 0 && path[i - 1] == '/') i--;
    if (i == 0) {
        filename[0] = '\0';
        return;
    }

    size_t end = i - 1;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/') start--;

    size_t j = 0;
    for (size_t k = start; k <= end; k++) {
        filename[j++] = path[k];
    }
    filename[j] = '\0';
}

static void extract_parent_path(const char *path, char *parent) {
    size_t len = strlen(path);
    size_t i = len - 1;
    while (i > 0 && path[i] == '/') i--;
    while (i > 0 && path[i] != '/') i--;
    if (i <= 0) {
        parent[0] = '/';
        parent[1] = 0;
    } else {
        for (size_t j = 0; j < i; j++) {
            parent[j] = path[j];
        }
        parent[i] = 0;
    }
}

void fat32_normalize_path(const char *path, char *normalized) {
    char *temp = (char *)kmalloc(FAT32_MAX_PATH);
    if (!temp) {
        if (normalized) normalized[0] = 0;
        return;
    }
    size_t temp_len = 0;
    const char *p = path;

    if (p[0] == '/') {
        strcpy(temp, "/");
        temp_len = 1;
    } else {
        strcpy(temp, "/");
        temp_len = 1;
    }

    size_t i = 0;
    while (p[i]) {
        while (p[i] == '/') i++;
        if (!p[i]) break;
        char component[FAT32_MAX_FILENAME];
        size_t j = 0;
        while (p[i] && p[i] != '/' && j < (FAT32_MAX_FILENAME - 1)) {
            component[j++] = p[i++];
        }
        component[j] = 0;

        if (strcmp(component, ".") == 0) {
            continue;
        } else if (strcmp(component, "..") == 0) {
            if (temp_len > 1) {
                while (temp_len > 0 && temp[temp_len - 1] != '/') temp_len--;
                if (temp_len > 1) temp_len--;
                temp[temp_len] = 0;
            }
        } else {
            size_t comp_len = strlen(component);
            if (temp_len + comp_len + 2 < FAT32_MAX_PATH) {
                if (temp[temp_len - 1] != '/') {
                    temp[temp_len++] = '/';
                    temp[temp_len] = 0;
                }
                strcat(temp, component);
                temp_len = strlen(temp);
            }
        }
    }
    if (temp_len > 1 && temp[temp_len - 1] == '/') temp[--temp_len] = 0;
    strcpy(normalized, temp);
    kfree_null(temp);
}

static fat32_file_handle_t *fat32_alloc_handle(void) {
    fat32_file_handle_t *fh = (fat32_file_handle_t *)kmalloc(sizeof(fat32_file_handle_t));
    if (!fh) return NULL;
    memset(fh, 0, sizeof(fat32_file_handle_t));
    return fh;
}

static bool fat32_mount_vol(fat32_volume_t *vol, Disk *disk) {
    if (vol->mounted) return true;
    
    uint32_t part_offset = 0;
    uint8_t *sect0 = (uint8_t *)kmalloc(512);
    if (!sect0) return false;
    
    if (disk->read_sector(disk, part_offset, sect0) != 0) {
        kfree_null(sect0);
        return false;
    }
    
    fat32_boot_sector_t *bpb = (fat32_boot_sector_t *)sect0;
    if (bpb->boot_signature_value != 0xAA55) {
        kfree_null(sect0);
        return false;
    }
    
    vol->disk = disk;
    vol->partition_offset = disk->partition_lba_offset;
    vol->fat_begin_lba = part_offset + bpb->reserved_sectors;
    vol->cluster_begin_lba = part_offset + bpb->reserved_sectors + (bpb->num_fats * bpb->sectors_per_fat_32);
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->root_cluster = bpb->root_cluster;
    vol->fat_size = bpb->sectors_per_fat_32;
    vol->num_fats = bpb->num_fats;
    vol->total_sectors = bpb->total_sectors_32;
    vol->mounted = true;
    vol->cached_fat_sector = 0xFFFFFFFF;
    vol->last_allocated_cluster = 2;
    
    serial_write("[FAT32] Mounted volume: /dev/");
    serial_write(disk->devname);
    serial_write("\n");
    
    kfree_null(sect0);
    return true;
}

static void fat32_update_dir_entry_size(fat32_volume_t *vol, fat32_file_handle_t *handle) {
    if (handle->is_directory) return;
    if (handle->dir_sector != 0 && handle->dir_offset != 0xFFFFFFFF && handle->dir_offset < 512) {
        uint8_t *dir_buf = (uint8_t *)kmalloc(512);
        if (dir_buf && vol->disk->read_sector(vol->disk, handle->dir_sector, dir_buf) == 0) {
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(dir_buf + handle->dir_offset);
            if (handle->start_cluster != 0) {
                entry->start_cluster_high = (handle->start_cluster >> 16);
                entry->start_cluster_low = (handle->start_cluster & 0xFFFF);
            }
            entry->file_size = handle->size;
            vol->disk->write_sector(vol->disk, handle->dir_sector, dir_buf);
        }
        kfree_null(dir_buf);
    }
}

static uint32_t fat32_next_cluster(fat32_volume_t *vol, uint32_t cluster) {
    uint32_t fat_sector = vol->fat_begin_lba + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    
    if (vol->cached_fat_sector != fat_sector) {
        if (vol->disk->read_sector(vol->disk, fat_sector, vol->cached_fat_buf) != 0) {
            return 0xFFFFFFFF;
        }
        vol->cached_fat_sector = fat_sector;
    }
    
    uint32_t next = *(uint32_t *)&vol->cached_fat_buf[fat_offset];
    next &= 0x0FFFFFFF;
    
    if (next == 0 || next == cluster) return 0x0FFFFFFF;
    return next;
}

static void fat32_set_fat_entry(fat32_volume_t *vol, uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint32_t sector_offset = offset / 512;
    uint32_t byte_offset = offset % 512;

    uint8_t buf[512];

    for (uint32_t i = 0; i < vol->num_fats; i++) {
        uint32_t sector = vol->fat_begin_lba + (i * vol->fat_size) + sector_offset;
        bool read_ok = false;

        if (i == 0 && vol->cached_fat_sector == sector) {
            memcpy(buf, vol->cached_fat_buf, 512);
            read_ok = true;
        } else {
            read_ok = (vol->disk->read_sector(vol->disk, sector, buf) == 0);
        }

        if (read_ok) {
            uint32_t *entry = (uint32_t *)(buf + byte_offset);
            *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
            vol->disk->write_sector(vol->disk, sector, buf);
            if (i == 0) {
                memcpy(vol->cached_fat_buf, buf, 512);
                vol->cached_fat_sector = sector;
            }
        }
    }
}



static void to_dos_filename(const char *filename, char *out) {
    for (size_t i = 0; i < 11; i++) out[i] = ' ';

    size_t len = strlen(filename);
    size_t dot = len;
    bool dot_exists = false;

    for (size_t i = len; i > 0; i--) {
        if (filename[i - 1] == '.') { 
          dot = i - 1;
          dot_exists = true; 
          break; 
        }
    }

    size_t name_len = (dot_exists) ? dot : len;
    if (name_len > 8) name_len = 8;

    for (size_t i = 0; i < name_len; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    if (dot_exists && dot + 1 < len) {
        size_t ext_len = len - dot - 1;
        if (ext_len > 3) ext_len = 3;
        for (size_t i = 0; i < ext_len; i++) {
            char c = filename[dot + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = c;
        }
    }
}

static uint8_t fat_lfn_checksum(const uint8_t *short_name) {
    uint8_t sum = 0;
    for (int i = 11; i > 0; i--) {
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + *short_name++;
    }
    return sum;
}

static bool fat32_create_entry(fat32_volume_t *vol, uint32_t parent_cluster, const char *name, uint8_t attributes, uint32_t start_cluster, uint32_t file_size, uint32_t *out_sector, uint32_t *out_offset) {
    char dos_name[11];
    to_dos_filename(name, dos_name);

    size_t name_len = strlen(name);
    bool needs_lfn = false;

    size_t dot_pos = name_len;
    bool dot_exists = false;

    for (size_t i = 0; i < name_len; i++) {
        if (name[i] == '.') { 
            dot_pos = i; 
            dot_exists = true; 
            break; 
        }
        if (name[i] >= 'a' && name[i] <= 'z') needs_lfn = true;
    }

    if (!needs_lfn) {
        if (!dot_exists) needs_lfn = (name_len > 8);
        else needs_lfn = (dot_pos > 8) || (name_len - dot_pos - 1 > 3);
    }

    if (!needs_lfn && dot_exists) {
        for (size_t i = dot_pos + 1; i < name_len; i++) {
            if (name[i] >= 'a' && name[i] <= 'z') { needs_lfn = true; break; }
        }
    }

    size_t lfn_entries = needs_lfn ? ((name_len + 12) / 13) : 0;
    size_t total_entries = lfn_entries + 1;

    uint32_t free_cluster = 0;
    size_t start_idx = 0;

    if (!fat32_find_contiguous_free(vol, parent_cluster, (uint32_t)total_entries, &free_cluster, &start_idx)) {
        return false;
    }

    uint8_t *buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
    if (!buf) return false;

    if (fat32_read_cluster(vol, free_cluster, buf) != 0) {
        kfree_null(buf);
        return false;
    }

    fat32_dir_entry_t *entries = (fat32_dir_entry_t *)buf;
    uint8_t checksum = fat_lfn_checksum((uint8_t *)dos_name);

    for (size_t i = 0; i < lfn_entries; i++) {
        fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entries[start_idx + i];

        lfn->order = (lfn_entries - i);
        if (i == 0) lfn->order |= 0x40;
        lfn->attr = ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->first_cluster = 0;

        size_t char_offset = (lfn_entries - i - 1) * 13;

        for (size_t k = 0; k < 13; k++) {
            uint16_t c = 0xFFFF;
            if (char_offset + k < name_len) c = (uint8_t)name[char_offset + k];
            else if (char_offset + k == name_len) c = 0x0000;

            if (k < 5) lfn->name1[k] = c;
            else if (k < 11) lfn->name2[k - 5] = c;
            else lfn->name3[k - 11] = c;
        }
    }

    fat32_dir_entry_t *d = &entries[start_idx + lfn_entries];

    for (size_t k = 0; k < 8; k++) d->filename[k] = dos_name[k];
    for (size_t k = 0; k < 3; k++) d->extension[k] = dos_name[8 + k];

    d->attributes = attributes;
    d->start_cluster_high = (start_cluster >> 16);
    d->start_cluster_low = (start_cluster & 0xFFFF);
    d->file_size = file_size;

    if (fat32_write_cluster(vol, free_cluster, buf) != 0) {
        kfree_null(buf);
        return false;
    }

    uint32_t lba = vol->cluster_begin_lba + (free_cluster - 2) * vol->sectors_per_cluster;
    *out_sector = lba + ((start_idx + lfn_entries) * 32) / 512;
    *out_offset = ((start_idx + lfn_entries) * 32) % 512;

    kfree_null(buf);
    return true;
}

static fat32_file_handle_t *fat32_open_vol(fat32_volume_t *vol, const char *path, const char *mode) {
    if (!vol || !vol->mounted) return NULL;
    
    uint32_t current_cluster = vol->root_cluster;
    const char *p = path;
    if (*p == '/') p++;
    
    if (*p == 0) {
        if (mode[0] == 'w') return NULL;
        fat32_file_handle_t *fh = fat32_alloc_handle();
        if (fh) {
            fh->valid = true;
            fh->volume = vol;
            fh->start_cluster = vol->root_cluster;
            fh->cluster = vol->root_cluster;
            fh->position = 0;
            fh->size = 0;
            fh->mode = 0;
            fh->is_directory = true;
            fh->attributes = ATTR_DIRECTORY;
            return fh;
        }
        return NULL;
    }
    
    char component[256];
    bool found = false;
    uint32_t file_size = 0;
    uint8_t attributes = 0;
    bool is_directory = false;
    
    uint32_t entry_sector = 0;
    uint32_t entry_offset = 0;
    
    uint8_t *cluster_buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return NULL;

    while (*p) {
        int i = 0;
        while (*p && *p != '/') {
            component[i++] = *p++;
        }
        component[i] = 0;
        if (*p == '/') p++;
        
        found = false;
        uint32_t search_cluster = current_cluster;
        
        char lfn_buffer[256];
        bool has_lfn = false;
        for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;

        while (search_cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(vol, search_cluster, cluster_buf) != 0) break;
            
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)cluster_buf;
            int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
            
            for (int e = 0; e < entries_per_cluster; e++) {
                if (entry[e].filename[0] == 0) break;
                
                if (entry[e].filename[0] == 0xE5) {
                    has_lfn = false;
                    continue;
                }
                
                if (entry[e].attributes == ATTR_LFN) {
                    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entry[e];
                    if (lfn->order & 0x40) {
                        for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;
                    }
                    extract_lfn_chars(lfn, lfn_buffer);
                    has_lfn = true;
                    continue;
                }
                
                if (entry[e].attributes & ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue;
                }
                
                char name[256];
                fat32_resolve_entry_name(&entry[e], lfn_buffer, has_lfn, name);
                has_lfn = false;

                if (fat32_name_match(name, component)) {
                    uint32_t cluster = (entry[e].start_cluster_high << 16) | entry[e].start_cluster_low;
                    
                    uint32_t lba = vol->cluster_begin_lba + (search_cluster - 2) * vol->sectors_per_cluster;
                    int sect_in_cluster = (e * 32) / 512;
                    entry_sector = lba + sect_in_cluster;
                    entry_offset = (e * 32) % 512;

                    if (*p == 0) {
                        current_cluster = cluster;
                        file_size = entry[e].file_size;
                        attributes = entry[e].attributes;
                        is_directory = (attributes & ATTR_DIRECTORY) != 0;
                        found = true;
                    } else {
                        if (entry[e].attributes & ATTR_DIRECTORY) {
                            current_cluster = cluster;
                            found = true;
                        }
                    }
                    break;
                }
            }
            if (found) break;
            search_cluster = fat32_next_cluster(vol, search_cluster);
        }
        
        if (!found) {
            if ((mode[0] == 'w' || mode[0] == 'a') && *p == 0) {
                if (fat32_create_entry(vol, current_cluster, component, ATTR_ARCHIVE, 0, 0, &entry_sector, &entry_offset)) {
                    fat32_file_handle_t *fh = fat32_alloc_handle();
                    if (fh) {
                        fh->valid = true;
                        fh->volume = vol;
                        fh->start_cluster = 0;
                        fh->cluster = 0;
                        fh->position = 0;
                        fh->size = 0;
                        fh->mode = (mode[0] == 'a' ? 2 : 1);
                        fh->is_directory = false;
                        fh->attributes = ATTR_ARCHIVE;
                        fh->dir_sector = entry_sector;
                        fh->dir_offset = entry_offset;
                        kfree_null(cluster_buf);
                        return fh;
                    }
                }
                kfree_null(cluster_buf);
                return NULL; 
            }
            kfree_null(cluster_buf);
            return NULL;
        }
    }

    fat32_file_handle_t *fh = fat32_alloc_handle();
    if (fh) {
        fh->valid = true;
        fh->volume = vol;
        fh->start_cluster = current_cluster;
        fh->cluster = current_cluster;
        fh->position = 0;
        fh->size = file_size;
        fh->mode = (mode[0] == 'w' ? 1 : (mode[0] == 'a' ? 2 : 0));
        fh->is_directory = is_directory;
        fh->attributes = attributes;
        fh->dir_sector = entry_sector;
        fh->dir_offset = entry_offset;
        
        if (mode[0] == 'w' && !is_directory) {
            fat32_truncate(fh);
        }
        
        if (mode[0] == 'a') {
            fh->position = fh->size;
            uint32_t cluster_size = vol->sectors_per_cluster * 512;
            uint32_t pos = 0;
            while (pos + cluster_size <= fh->position) {
                uint32_t next = fat32_next_cluster(vol, fh->cluster);
                if (next >= 0x0FFFFFF8) break;
                fh->cluster = next;
                pos += cluster_size;
            }
        }
    }
    kfree_null(cluster_buf);
    return fh;
}

static int fat32_read_vol(fat32_file_handle_t *handle, void *buffer, size_t size, uint8_t *ext_cluster_buf) {
    fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
    if (!vol) return 0;
    
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = ext_cluster_buf;
    
    if (!cluster_buf) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) return 0;
    }
    
    int bytes_read = 0;
    uint8_t *out_buf = (uint8_t *)buffer;
    
    while ((size_t)bytes_read < size && handle->position < handle->size) {
        if (fat32_read_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        
        uint32_t offset = handle->position % cluster_size;
        int to_copy = size - bytes_read;
        int available = cluster_size - offset;
        if ((int)(handle->size - handle->position) < available) available = handle->size - handle->position;
        if (to_copy > available) to_copy = available;
        
        for (int i = 0; i < to_copy; i++) {
            out_buf[bytes_read + i] = cluster_buf[offset + i];
        }
        
        bytes_read += to_copy;
        handle->position += to_copy;
        
        if (handle->position % cluster_size == 0 && handle->position < handle->size) {
            handle->cluster = fat32_next_cluster(vol, handle->cluster);
            if (handle->cluster >= 0x0FFFFFF8) break;
        }
    }
    
    if (!ext_cluster_buf) {
        kfree_null(cluster_buf);
    }
    return bytes_read;
}

static int fat32_read_cluster(fat32_volume_t *vol, uint32_t cluster, uint8_t *buffer) {
    if (!vol || cluster < 2 || cluster >= 0x0FFFFFF8) return -1;
    uint32_t lba = vol->cluster_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
    if (vol->disk->read_sectors) {
        if (vol->disk->read_sectors(vol->disk, lba, vol->sectors_per_cluster, buffer) == 0) {
            return 0;
        }
    }
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        if (vol->disk->read_sector(vol->disk, lba + i, buffer + (i * 512)) != 0) return -1;
    }
    return 0;
}

static int fat32_write_cluster(fat32_volume_t *vol, uint32_t cluster, const uint8_t *buffer) {
    if (!vol || cluster < 2 || cluster >= 0x0FFFFFF8) return -1;
    uint32_t lba = vol->cluster_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
    if (vol->disk->write_sectors) {
        if (vol->disk->write_sectors(vol->disk, lba, vol->sectors_per_cluster, buffer) == 0) {
            return 0;
        }
    }
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        if (vol->disk->write_sector(vol->disk, lba + i, buffer + (i * 512)) != 0) return -1;
    }
    return 0;
}

static bool fat32_find_contiguous_free(fat32_volume_t *vol, uint32_t dir_start_cluster, int n, uint32_t *out_cluster, size_t *out_entry_idx) {
    uint32_t current = dir_start_cluster;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return false;
    
    int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
    
    while (current < 0x0FFFFFF8) {
        if (fat32_read_cluster(vol, current, cluster_buf) != 0) break;
        fat32_dir_entry_t *entries = (fat32_dir_entry_t *)cluster_buf;
        
        int contiguous = 0;
        int start_idx = -1;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].filename[0] == 0 || entries[i].filename[0] == 0xE5) {
                if (contiguous == 0) start_idx = i;
                contiguous++;
                if (contiguous >= n) {
                    *out_cluster = current;
                    *out_entry_idx = start_idx;
                    kfree_null(cluster_buf);
                    return true;
                }
            } else {
                contiguous = 0;
            }
        }
        
        uint32_t next = fat32_next_cluster(vol, current);
        if (next >= 0x0FFFFFF8) {
            uint32_t new_cluster = fat32_allocate_cluster(vol);
            if (new_cluster != 0) {
                fat32_set_fat_entry(vol, current, new_cluster);
                
                uint8_t *zero_buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
                if (zero_buf) {
                    for (uint32_t k = 0; k < vol->sectors_per_cluster * 512; k++) zero_buf[k] = 0;
                    fat32_write_cluster(vol, new_cluster, zero_buf);
                    kfree_null(zero_buf);
                }
                
                next = new_cluster;
            }
        }
        current = next;
    }
    
    kfree_null(cluster_buf);
    return false;
}

static uint32_t fat32_allocate_cluster(fat32_volume_t *vol) {
    uint32_t current = vol->last_allocated_cluster;
    if (current < 3) current = 3;
    
    uint32_t fat_entries = (vol->fat_size * 512) / 4;
    uint32_t first_search = current;
    
    while (current < fat_entries) {
        uint32_t sector = vol->fat_begin_lba + (current * 4) / 512;
        uint32_t offset = (current * 4) % 512;
        
        if (vol->cached_fat_sector != sector) {
            if (vol->disk->read_sector(vol->disk, sector, vol->cached_fat_buf) != 0) {
                return 0;
            }
            vol->cached_fat_sector = sector;
        }
        
        uint32_t val = *(uint32_t *)&vol->cached_fat_buf[offset];
        if ((val & 0x0FFFFFFF) == 0) {
            fat32_set_fat_entry(vol, current, 0x0FFFFFFF);
            vol->last_allocated_cluster = current;
            return current;
        }
        current++;
        if (current >= fat_entries) current = 2;
        if (current == first_search) break; 
    }
    return 0;
}

static void fat32_free_cluster_chain(fat32_volume_t *vol, uint32_t start_cluster) {
    if (start_cluster == 0 || start_cluster >= 0x0FFFFFF8) return;
    
    uint32_t current = start_cluster;
    while (current < 0x0FFFFFF8 && current >= 2) {
        uint32_t next = fat32_next_cluster(vol, current);
        fat32_set_fat_entry(vol, current, 0);
        if (next == current) break; 
        current = next;
    }
}

static void fat32_truncate(fat32_file_handle_t *handle) {
    fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
    if (!vol || handle->start_cluster == 0) return;
    
    uint32_t start = handle->start_cluster;
    handle->start_cluster = 0;
    handle->cluster = 0;
    handle->size = 0;
    handle->position = 0;
    
    fat32_free_cluster_chain(vol, start);
    fat32_update_dir_entry_size(vol, handle);
}

static int fat32_write_vol(fat32_file_handle_t *handle, const void *buffer, size_t size, uint8_t *ext_cluster_buf) {
    fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
    if (!vol) return 0;
    
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = ext_cluster_buf;
    
    if (!cluster_buf) {
        cluster_buf = (uint8_t *)kmalloc(cluster_size);
        if (!cluster_buf) return 0;
    }
    
    int bytes_written = 0;
    const uint8_t *src_buf = (const uint8_t *)buffer;
    
    if (handle->cluster == 0) {
        uint32_t new_cluster = fat32_allocate_cluster(vol);
        if (new_cluster == 0) {
            if (!ext_cluster_buf) kfree_null(cluster_buf);
            return 0;
        }
        handle->start_cluster = new_cluster;
        handle->cluster = new_cluster;
        handle->position = 0;
        handle->size = 0;
        fat32_update_dir_entry_size(vol, handle);
    }
    
    while ((size_t)bytes_written < size) {
        uint32_t offset = handle->position % cluster_size;

        if (offset == 0 && handle->position > 0) {
            uint32_t next = fat32_next_cluster(vol, handle->cluster);
            if (next >= 0x0FFFFFF8) {
                uint32_t new_cluster = fat32_allocate_cluster(vol);
                if (new_cluster == 0) break;

                fat32_set_fat_entry(vol, handle->cluster, new_cluster);
                next = new_cluster;
            }
            handle->cluster = next;
        }

        int remaining = size - bytes_written;
        if (offset == 0 && remaining >= (int)cluster_size && vol->disk->write_sectors) {
            int clusters_to_write = remaining / cluster_size;
            if (clusters_to_write > 64) clusters_to_write = 64;

            int count = 1;
            uint32_t current_c = handle->cluster;

            for (int i = 1; i < clusters_to_write; i++) {
                uint32_t next = fat32_next_cluster(vol, current_c);
                if (next >= 0x0FFFFFF8) {
                    uint32_t new_c = fat32_allocate_cluster(vol);
                    if (new_c == 0) break;
                    fat32_set_fat_entry(vol, current_c, new_c);
                    next = new_c;
                }
                if (next != current_c + 1) {
                    break;
                }
                current_c = next;
                count++;
            }

            uint32_t lba = vol->cluster_begin_lba + (handle->cluster - 2) * vol->sectors_per_cluster;
            uint32_t total_sectors = count * vol->sectors_per_cluster;
            if (vol->disk->write_sectors(vol->disk, lba, total_sectors, src_buf + bytes_written) != 0) {
                uint32_t cluster_to_write = handle->cluster;
                bool write_ok = true;
                for (int c = 0; c < count; c++) {
                    if (fat32_write_cluster(vol, cluster_to_write, src_buf + bytes_written + c * cluster_size) != 0) {
                        write_ok = false;
                        break;
                    }
                    if (c + 1 < count) {
                        cluster_to_write = fat32_next_cluster(vol, cluster_to_write);
                    }
                }
                if (!write_ok) break;
            }

            uint32_t written_len = count * cluster_size;
            bytes_written += written_len;
            handle->position += written_len;
            if (handle->position > handle->size) handle->size = handle->position;
            handle->cluster = current_c;
            continue;
        }

        int to_copy = remaining;
        int available = cluster_size - offset; 
        if (to_copy > available) to_copy = available;

        if (offset > 0 || (handle->position < handle->size && (handle->position + to_copy) < handle->size)) {
            if (fat32_read_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        } else {
            if (to_copy < (int)cluster_size) {
                for (int i = 0; i < (int)cluster_size; i++) cluster_buf[i] = 0;
            }
        }
        
        for (int i = 0; i < to_copy; i++) {
            cluster_buf[offset + i] = src_buf[bytes_written + i];
        }
        
        if (fat32_write_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        
        bytes_written += to_copy;
        handle->position += to_copy;
        if (handle->position > handle->size) handle->size = handle->position;
    }
    
    if (bytes_written > 0) fat32_update_dir_entry_size(vol, handle);
    if (!ext_cluster_buf) {
        kfree_null(cluster_buf);
    }
    return bytes_written;
}

static bool fat32_delete_vol(fat32_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted) return false;
    
    uint32_t current_cluster = vol->root_cluster;
    const char *p = path;
    if (*p == '/') p++;
    
    if (*p == 0) return false;
    
    char component[256];
    uint32_t file_start_cluster = 0;
    bool is_directory = false;
    
    uint8_t *cluster_buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return false;
    
    while (*p) {
        int i = 0;
        while (*p && *p != '/') {
            component[i++] = *p++;
        }
        component[i] = 0;
        if (*p == '/') p++;
        
        bool found = false;
        uint32_t search_cluster = current_cluster;
        
        char lfn_buffer[256];
        bool has_lfn = false;
        for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;

        while (search_cluster < 0x0FFFFFF8) {
            if (fat32_read_cluster(vol, search_cluster, cluster_buf) != 0) break;
            
            fat32_dir_entry_t *entry = (fat32_dir_entry_t *)cluster_buf;
            int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
            
            for (int e = 0; e < entries_per_cluster; e++) {
                if (entry[e].filename[0] == 0) break;
                
                if (entry[e].filename[0] == 0xE5) {
                    has_lfn = false;
                    continue;
                }
                
                if (entry[e].attributes == ATTR_LFN) {
                    fat32_lfn_entry_t *lfn = (fat32_lfn_entry_t *)&entry[e];
                    if (lfn->order & 0x40) {
                        for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;
                    }
                    extract_lfn_chars(lfn, lfn_buffer);
                    has_lfn = true;
                    continue;
                }
                
                if (entry[e].attributes & ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue;
                }
                
                char name[256];
                fat32_resolve_entry_name(&entry[e], lfn_buffer, has_lfn, name);
                has_lfn = false;

                bool match = fat32_name_match(name, component);

                int lfn_start_entry = -1;
                if (has_lfn) {
                    for (int k = e - 1; k >= 0; k--) {
                        if (entry[k].attributes == ATTR_LFN) {
                            lfn_start_entry = k;
                        } else {
                            if (entry[k].filename[0] != 0xE5) break;
                        }
                    }
                }

                if (match) {
                    file_start_cluster = (entry[e].start_cluster_high << 16) | entry[e].start_cluster_low;
                    is_directory = (entry[e].attributes & ATTR_DIRECTORY) != 0;
                    
                    if (*p == 0) {
                        if (lfn_start_entry != -1) {
                            for (int k = lfn_start_entry; k < e; k++) {
                                entry[k].filename[0] = 0xE5;
                            }
                        }
                        entry[e].filename[0] = 0xE5;
                        uint32_t lba = vol->cluster_begin_lba + (search_cluster - 2) * vol->sectors_per_cluster;
                        
                        uint8_t sectors_to_write[8] = {0};  
                        int num_sectors = 0;
                        if (lfn_start_entry != -1) {
                            for (int k = lfn_start_entry; k < e; k++) {
                                int sect_idx = (k * 32) / 512;
                                bool already_marked = false;
                                for (int s = 0; s < num_sectors; s++) {
                                    if (sectors_to_write[s] == sect_idx) {
                                        already_marked = true;
                                        break;
                                    }
                                }
                                if (!already_marked && num_sectors < 8) {
                                    sectors_to_write[num_sectors++] = sect_idx;
                                }
                            }
                        }
                        
                        int main_sect_idx = (e * 32) / 512;
                        bool already_marked = false;
                        for (int s = 0; s < num_sectors; s++) {
                            if (sectors_to_write[s] == main_sect_idx) {
                                already_marked = true;
                                break;
                            }
                        }
                        if (!already_marked && num_sectors < 8) {
                            sectors_to_write[num_sectors++] = main_sect_idx;
                        }
                        
                        for (int s = 0; s < num_sectors; s++) {
                            int sect_idx = sectors_to_write[s];
                            vol->disk->write_sector(vol->disk, lba + sect_idx, ((uint8_t *)entry) + (sect_idx * 512));
                        }
                        
                        found = true;
                    } else {
                        if (is_directory) {
                            current_cluster = file_start_cluster;
                            found = true;
                        }
                    }
                    break;
                }
            }
            if (found) break;
            search_cluster = fat32_next_cluster(vol, search_cluster);
        }
        
        if (!found) {
            kfree_null(cluster_buf);
            return false;
        }
        
        if (*p == 0) break;
    }
    fat32_free_cluster_chain(vol, file_start_cluster);

    kfree_null(cluster_buf);
    return true;
}

static void extract_lfn_chars(fat32_lfn_entry_t *lfn, char *buffer) {
    int order = lfn->order & 0x1F;
    if (order < 1 || order > 20) return;
    int offset = (order - 1) * 13;
    
    for (int i = 0; i < 5; i++) {
        uint16_t c = lfn->name1[i];
        if (c == 0x0000 || c == 0xFFFF) { buffer[offset] = 0; return; }
        buffer[offset++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 6; i++) {
        uint16_t c = lfn->name2[i];
        if (c == 0x0000 || c == 0xFFFF) { buffer[offset] = 0; return; }
        buffer[offset++] = (char)(c & 0xFF);
    }
    for (int i = 0; i < 2; i++) {
        uint16_t c = lfn->name3[i];
        if (c == 0x0000 || c == 0xFFFF) { buffer[offset] = 0; return; }
        buffer[offset++] = (char)(c & 0xFF);
    }
}

static int fat32_list_directory_vol(fat32_volume_t *vol, const char *path, fat32_file_info_t *entries, int max_entries, int offset) {
    if (!vol || !vol->mounted) return 0;
    fat32_file_handle_t *dir_handle = fat32_open_vol(vol, path, "r");
    if (!dir_handle) return 0;
    uint32_t current_cluster = dir_handle->start_cluster;
    extern void fat32_close_nolock(fat32_file_handle_t *handle);
    fat32_close_nolock(dir_handle);
    
    int count = 0;
    int found_so_far = 0;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(vol->sectors_per_cluster * 512);
    char *lfn_buffer = (char *)kmalloc(256);
    char *name = (char *)kmalloc(256);
    if (!cluster_buf || !lfn_buffer || !name) {
       kfree_null(cluster_buf);
       kfree_null(lfn_buffer);
       kfree_null(name);
       return 0;
    }
    bool has_lfn = false;
    for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;

    while (current_cluster < 0x0FFFFFF8 && count < max_entries) {
        if (fat32_read_cluster(vol, current_cluster, cluster_buf) != 0) break;
        fat32_dir_entry_t *de = (fat32_dir_entry_t *)cluster_buf;
        for (int e = 0; e < (int)((vol->sectors_per_cluster * 512) / 32) && count < max_entries; e++) {
            if (de[e].filename[0] == 0) { current_cluster = 0xFFFFFFFF; break; }
            if (de[e].filename[0] == 0xE5) { has_lfn = false; continue; }
            if (de[e].filename[0] == 0x2E) { has_lfn = false; continue; }
            if (de[e].attributes == ATTR_LFN) {
                fat32_lfn_entry_t *l = (fat32_lfn_entry_t *)&de[e];
                if (l->order & 0x40) for (int k = 0; k < 256; k++) lfn_buffer[k] = 0;
                extract_lfn_chars(l, lfn_buffer);
                has_lfn = true; continue;
            }
            if (de[e].attributes & ATTR_VOLUME_ID) { has_lfn = false; continue; }
            
            if (found_so_far >= offset) {
                if (has_lfn && lfn_buffer[0] != 0) strcpy(entries[count].name, lfn_buffer);
                else {
                    int n = 0;
                    for (int k = 0; k < 8 && de[e].filename[k] != ' '; k++) entries[count].name[n++] = de[e].filename[k];
                    if (de[e].extension[0] != ' ') {
                        entries[count].name[n++] = '.';
                        for (int k = 0; k < 3 && de[e].extension[k] != ' '; k++) entries[count].name[n++] = de[e].extension[k];
                    }
                    entries[count].name[n] = 0;
                }
                entries[count].size = de[e].file_size;
                entries[count].is_directory = (de[e].attributes & ATTR_DIRECTORY) != 0;
                entries[count].start_cluster = (de[e].start_cluster_high << 16) | de[e].start_cluster_low;
                count++;
            }
            found_so_far++;
            has_lfn = false;
        }
        if (current_cluster < 0x0FFFFFF8) current_cluster = fat32_next_cluster(vol, current_cluster);
    }
    kfree_null(cluster_buf); kfree_null(lfn_buffer); kfree_null(name);
    return count;
}

static bool fat32_mkdir_vol(fat32_volume_t *vol, const char *path) {
    if (!vol || !vol->mounted) return false;

    char *parent_path = (char *)kmalloc(FAT32_MAX_PATH);
    if (!parent_path) return false;
    char dirname[FAT32_MAX_FILENAME];
    extract_parent_path(path, parent_path);
    extract_filename(path, dirname);

    fat32_file_handle_t *parent_fh = fat32_open_vol(vol, parent_path, "r");
    kfree_null(parent_path);
    if (!parent_fh) return false;
    uint32_t parent_cluster = parent_fh->start_cluster;

    extern void fat32_close_nolock(fat32_file_handle_t *handle);
    fat32_close_nolock(parent_fh);

    fat32_file_handle_t *check_fh = fat32_open_vol(vol, path, "r");
    if (check_fh) {
        fat32_close_nolock(check_fh);
        return false;
    }

    uint32_t new_cluster = fat32_allocate_cluster(vol);
    if (new_cluster == 0) return false;

    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return false;
    for (uint32_t i = 0; i < cluster_size; i++) cluster_buf[i] = 0;

    fat32_dir_entry_t *dot = (fat32_dir_entry_t *)cluster_buf;
    fat32_dir_entry_t *dotdot = (fat32_dir_entry_t *)(cluster_buf + 32);

    for (int i = 0; i < 8; i++) dot->filename[i] = ' ';
    for (int i = 0; i < 3; i++) dot->extension[i] = ' ';
    dot->filename[0] = '.';
    dot->attributes = ATTR_DIRECTORY;
    dot->start_cluster_high = (new_cluster >> 16);
    dot->start_cluster_low = (new_cluster & 0xFFFF);

    for (int i = 0; i < 8; i++) dotdot->filename[i] = ' ';
    for (int i = 0; i < 3; i++) dotdot->extension[i] = ' ';
    dotdot->filename[0] = '.'; dotdot->filename[1] = '.';
    dotdot->attributes = ATTR_DIRECTORY;
    dotdot->start_cluster_high = (parent_cluster >> 16);
    dotdot->start_cluster_low = (parent_cluster & 0xFFFF);

    if (fat32_write_cluster(vol, new_cluster, cluster_buf) != 0) {
        kfree_null(cluster_buf);
        return false;
    }
    kfree_null(cluster_buf);

    uint32_t free_sector = 0;
    uint32_t free_offset = 0;
    return fat32_create_entry(vol, parent_cluster, dirname, ATTR_DIRECTORY, new_cluster, 0, &free_sector, &free_offset);
}

static uint32_t vfs_fat_get_position(void *file_handle) {
    return ((fat32_file_handle_t *)file_handle)->position;
}

static uint32_t vfs_fat_get_size(void *file_handle) {
    return ((fat32_file_handle_t *)file_handle)->size;
}

static void *vfs_fat32_open(void *fs_private, const char *rel_path, const char *mode) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, mode);
    spinlock_release_irqrestore(&vol->lock, rflags);
    return fh;
}

static void vfs_fat32_close(void *fs_private, void *file_handle) {
    (void)fs_private;
    if (!file_handle) return;
    fat32_close((fat32_file_handle_t *)file_handle);
    kfree_null(file_handle);
}

static int vfs_fat32_read(void *fs_private, void *file_handle, void *buf, size_t size) {
    (void)fs_private;
    if (!buf && size > 0) return -1;
    fat32_file_handle_t *handle = (fat32_file_handle_t *)file_handle;
    if (!handle || !handle->volume) return -1;
    fat32_volume_t *vol = (fat32_volume_t *)handle->volume;

    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    size_t total_read = 0;
    while (total_read < size) {
        size_t to_read = size - total_read;
        if (to_read > cluster_size) to_read = cluster_size;

        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        int ret = fat32_read_vol(handle, (uint8_t *)buf + total_read, to_read, cluster_buf);
        spinlock_release_irqrestore(&vol->lock, rflags);

        if (ret <= 0) break;
        total_read += ret;
        if ((size_t)ret < to_read) break;
    }

    kfree_null(cluster_buf);
    if (total_read > INT_MAX) return -1;
    return (int)total_read;
}

static int vfs_fat32_write(void *fs_private, void *file_handle, const void *buf, size_t size) {
    (void)fs_private;
    if (!buf && size > 0) return -1;
    fat32_file_handle_t *handle = (fat32_file_handle_t *)file_handle;
    if (!handle || !handle->volume) return -1;
    fat32_volume_t *vol = (fat32_volume_t *)handle->volume;

    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t *)kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    int ret = fat32_write_vol(handle, buf, size, cluster_buf);
    spinlock_release_irqrestore(&vol->lock, rflags);

    if (ret > 0) fat32_sync_if_root(vol);
    kfree_null(cluster_buf);
    if (ret < 0) return -1;
    return ret;
}

static int vfs_fat32_seek(void *fs_private, void *file_handle, int offset, int whence) {
    (void)fs_private;
    return fat32_seek((fat32_file_handle_t *)file_handle, offset, whence);
}

static int vfs_fat32_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_info_t *fat_entries = (fat32_file_info_t *)kmalloc(max * sizeof(fat32_file_info_t));
    if (!fat_entries) { spinlock_release_irqrestore(&vol->lock, rflags); return 0; }
    
    int count = fat32_list_directory_vol(vol, rel_path, fat_entries, max, offset);
    for (int i = 0; i < count; i++) {
        strcpy(entries[i].name, fat_entries[i].name);
        entries[i].size = fat_entries[i].size;
        entries[i].is_directory = fat_entries[i].is_directory;
        entries[i].write_date = fat_entries[i].write_date;
        entries[i].write_time = fat_entries[i].write_time;
    }
    
    kfree_null(fat_entries);
    spinlock_release_irqrestore(&vol->lock, rflags);
    return count;
}

static bool vfs_fat32_mkdir(void *fs_private, const char *rel_path) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    bool ret = fat32_mkdir_vol(vol, rel_path);
    spinlock_release_irqrestore(&vol->lock, rflags);
    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_fat32_rmdir(void *fs_private, const char *rel_path) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    fat32_file_info_t child;
    bool ret = false;

    if (!vol || !rel_path || rel_path[0] == '\0' || strcmp(rel_path, "/") == 0) {
        return false;
    }

    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, "r");
    if (!fh) {
        spinlock_release_irqrestore(&vol->lock, rflags);
        return false;
    }

    bool is_dir = fh->is_directory;
    extern void fat32_close_nolock(fat32_file_handle_t *handle);
    fat32_close_nolock(fh);

    if (is_dir && fat32_list_directory_vol(vol, rel_path, &child, 1, 0) == 0) {
        ret = fat32_delete_vol(vol, rel_path);
    }
    spinlock_release_irqrestore(&vol->lock, rflags);

    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_fat32_unlink(void *fs_private, const char *rel_path) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);

    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, "r");
    if (!fh) {
        spinlock_release_irqrestore(&vol->lock, rflags);
        return false;
    }

    bool is_dir = fh->is_directory;
    extern void fat32_close_nolock(fat32_file_handle_t *handle);
    fat32_close_nolock(fh);
    kfree_null(fh);

    bool ret = is_dir ? false : fat32_delete_vol(vol, rel_path);
    spinlock_release_irqrestore(&vol->lock, rflags);
    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_fat32_rename(void *fs_private, const char *old_path, const char *new_path) {
    (void)fs_private; (void)old_path; (void)new_path;
    return false;
}

static bool vfs_fat32_exists(void *fs_private, const char *rel_path) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, "r");
    if (fh) {
        extern void fat32_close_nolock(fat32_file_handle_t *handle);
        fat32_close_nolock(fh);
        spinlock_release_irqrestore(&vol->lock, rflags);
        return true;
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return false;
}

static bool vfs_fat32_is_dir(void *fs_private, const char *rel_path) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    if (strcmp(rel_path, "/") == 0 || strcmp(rel_path, "") == 0) return true;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, "r");
    bool is_dir = false;
    if (fh) {
        is_dir = fh->is_directory;
        extern void fat32_close_nolock(fat32_file_handle_t *handle);
        fat32_close_nolock(fh);
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return is_dir; 
}

static int vfs_fat32_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    fat32_file_handle_t *fh = fat32_open_vol(vol, rel_path, "r");
    if (fh) {
        extract_filename(rel_path, info->name);
        info->size = fh->size;
        info->is_directory = fh->is_directory ? 1 : 0;
        extern void fat32_close_nolock(fat32_file_handle_t *handle);
        fat32_close_nolock(fh);
        spinlock_release_irqrestore(&vol->lock, rflags);
        return 0;
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return -1;
}

static int vfs_fat32_statfs(void *fs_private, vfs_statfs_t *stat) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    
    stat->total_blocks = vol->total_sectors / vol->sectors_per_cluster;
    stat->block_size = vol->sectors_per_cluster * 512;
    
    uint64_t free_count = 0;
    uint32_t fat_entries = (vol->fat_size * 512) / 4;
    uint32_t current = 2;
    
    uint8_t *fat_buf = (uint8_t *)kmalloc(512);
    if (fat_buf) {
        uint32_t cached_sector = 0xFFFFFFFF;
        while (current < fat_entries) {
            uint32_t sector = vol->fat_begin_lba + (current * 4) / 512;
            uint32_t offset = (current * 4) % 512;
            
            if (sector != cached_sector) {
                if (vol->disk->read_sector(vol->disk, sector, fat_buf) != 0) break;
                cached_sector = sector;
            }
            
            uint32_t val = *(uint32_t *)&fat_buf[offset];
            if ((val & 0x0FFFFFFF) == 0) free_count++;
            
            current++;
        }
        kfree_null(fat_buf);
    }
    
    stat->free_blocks = free_count;
    spinlock_release_irqrestore(&vol->lock, rflags);
    return 0;
}

int vfs_fat32_sync_fs(void *fs_private) {
    fat32_volume_t *vol = (fat32_volume_t *)fs_private;
    if (!vol || !vol->mounted) return 0;
    fat32_sync_if_root(vol);
    Disk *d = vol->disk;
    if (d && d->sync) d->sync(d);
    return 0;
}

static struct vfs_fs_ops fat32_ops = {
    .open = vfs_fat32_open,
    .close = vfs_fat32_close,
    .read = vfs_fat32_read,
    .write = vfs_fat32_write,
    .seek = vfs_fat32_seek,
    .readdir = vfs_fat32_readdir,
    .mkdir = vfs_fat32_mkdir,
    .rmdir = vfs_fat32_rmdir,
    .unlink = vfs_fat32_unlink,
    .rename = vfs_fat32_rename,
    .exists = vfs_fat32_exists,
    .is_dir = vfs_fat32_is_dir,
    .get_info = vfs_fat32_get_info,
    .get_position = vfs_fat_get_position,
    .get_size = vfs_fat_get_size,
    .statfs = vfs_fat32_statfs,
    .sync_fs = vfs_fat32_sync_fs
};

struct vfs_fs_ops *fat32_get_ops(void) {
    return &fat32_ops;
}

struct vfs_fs_ops *fat32_get_realfs_ops(void) {
    return &fat32_ops;
}

void *fat32_mount_volume(void *disk_ptr) {
    uint64_t flags = spinlock_acquire_irqsave(&fat32_subsystem_lock);
    Disk *disk = (Disk *)disk_ptr;

    for (int i = 0; i < fat32_volume_count; i++) {
        if (fat32_volumes[i] && fat32_volumes[i]->disk == disk) {
            fat32_volumes[i]->mounted = false;
            if (fat32_mount_vol(fat32_volumes[i], disk)) {
                spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
                return fat32_volumes[i];
            }
        }
    }

    if (fat32_volume_count >= MAX_FAT32_VOLUMES) {
        for (int i = 0; i < MAX_FAT32_VOLUMES; i++) {
            if (!fat32_volumes[i] || !fat32_volumes[i]->mounted) {
                if (!fat32_volumes[i]) {
                    fat32_volumes[i] = (fat32_volume_t *)kmalloc(sizeof(fat32_volume_t));
                }
                if (fat32_volumes[i]) {
                    fat32_volumes[i]->mounted = false;
                    fat32_volumes[i]->lock = SPINLOCK_INIT;
                    if (fat32_mount_vol(fat32_volumes[i], disk)) {
                        spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
                        return fat32_volumes[i];
                    }
                }
            }
        }
        spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
        return NULL;
    }
    
    fat32_volume_t *vol = (fat32_volume_t *)kmalloc(sizeof(fat32_volume_t));
    if (!vol) {
        spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
        return NULL;
    }
    
    vol->mounted = false;
    vol->lock = SPINLOCK_INIT;
    if (fat32_mount_vol(vol, disk)) {
        fat32_volumes[fat32_volume_count++] = vol;
        spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
        return vol;
    }
    
    kfree_null(vol);
    spinlock_release_irqrestore(&fat32_subsystem_lock, flags);
    return NULL;
}

void fat32_init(void) {
    for (int i = 0; i < MAX_FAT32_VOLUMES; i++) fat32_volumes[i] = NULL;
    fat32_volume_count = 0;
}

fat32_file_handle_t *fat32_open_nolock(const char *path, const char *mode) {
    if (path[0] == '/') {
        vfs_file_t *vf = vfs_open(path, mode);
        if (vf && vf->fs_handle) {
             return (fat32_file_handle_t *)vf->fs_handle;
        }
    }
    if (root_volume != NULL) {
        return fat32_open_vol(root_volume, path, mode);
    }
    return NULL;
}

fat32_file_handle_t *fat32_open(const char *path, const char *mode) {
    return fat32_open_nolock(path, mode);
}

void fat32_close_nolock(fat32_file_handle_t *handle) {
    if (handle && handle->valid) {
        if (handle->volume != NULL && handle->mode != 0) {
            fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
            Disk *d = vol->disk;
            if (d && handle->dir_sector != 0) {
                 uint8_t *buf = (uint8_t *)kmalloc(512);
                 if (buf) {
                     if (d->read_sector(d, handle->dir_sector, buf) == 0) {
                         fat32_dir_entry_t *entry = (fat32_dir_entry_t *)(buf + handle->dir_offset);
                         entry->file_size = handle->size;
                         if (handle->start_cluster != 0) {
                             entry->start_cluster_high = (handle->start_cluster >> 16);
                             entry->start_cluster_low = (handle->start_cluster & 0xFFFF);
                         }
                         d->write_sector(d, handle->dir_sector, buf);
                     }
                     kfree_null(buf);
                 }
            }
            fat32_sync_if_root(vol);
        }
        handle->valid = false;
    }
}

void fat32_close(fat32_file_handle_t *handle) {
    if (!handle) return;
    if (handle->volume) {
        fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        fat32_close_nolock(handle);
        spinlock_release_irqrestore(&vol->lock, rflags);
    } else {
        fat32_close_nolock(handle);
    }
}

int fat32_read(fat32_file_handle_t *handle, void *buffer, size_t size) {
    if (!handle || !handle->valid || handle->mode != 0) return -1;
    if (handle->volume != NULL) {
        fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        int ret = fat32_read_vol(handle, buffer, size, NULL);
        spinlock_release_irqrestore(&vol->lock, rflags);
        return ret;
    }
    return -1;
}

int fat32_write(fat32_file_handle_t *handle, const void *buffer, size_t size) {
    if (!handle || !handle->valid || (handle->mode != 1 && handle->mode != 2)) return -1;
    if (handle->volume != NULL) {
        fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        int ret = fat32_write_vol(handle, buffer, size, NULL);
        spinlock_release_irqrestore(&vol->lock, rflags);
        if (ret > 0) {
            fat32_sync_if_root(vol);
        }
        return ret;
    }
    return -1;
}

int fat32_seek(fat32_file_handle_t *handle, int offset, int whence) {
    if (!handle || !handle->valid) return -1;
    
    uint32_t new_position = handle->position;
    if (whence == 0) new_position = offset;
    else if (whence == 1) new_position += offset;
    else if (whence == 2) new_position = handle->size + offset;
    
    if (new_position > handle->size) new_position = handle->size;
    handle->position = new_position;
    
    if (handle->volume != NULL) {
        fat32_volume_t *vol = (fat32_volume_t *)handle->volume;
        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        uint32_t cluster_size = vol->sectors_per_cluster * 512;
        
        handle->cluster = handle->start_cluster;
        uint32_t pos = 0;
        while (pos + cluster_size <= handle->position) {
             uint32_t next = fat32_next_cluster(vol, handle->cluster);
             if (next >= 0x0FFFFFF8) break;
             handle->cluster = next;
             pos += cluster_size;
        }
        spinlock_release_irqrestore(&vol->lock, rflags);
    }
    
    return new_position;
}

bool fat32_mkdir(const char *path) {
    if (path[0] == '/') return vfs_mkdir(path);
    if (root_volume != NULL) {
        uint64_t rflags = spinlock_acquire_irqsave(&root_volume->lock);
        bool res = fat32_mkdir_vol(root_volume, path);
        spinlock_release_irqrestore(&root_volume->lock, rflags);
        return res;
    }
    return false;
}

void fat32_mkdir_recursive(const char *path) {
    char temp[256];
    int i = 0;
    if (path[0] == '/') {
        temp[0] = '/';
        i = 1;
    }
    while (path[i] && i < 255) {
        temp[i] = path[i];
        if (path[i] == '/') {
            temp[i] = '\0';
            fat32_mkdir(temp);
            temp[i] = '/';
        }
        i++;
    }
    if (i > 0 && temp[i - 1] != '/') {
        temp[i] = '\0';
        fat32_mkdir(temp);
    }
}

bool fat32_rmdir(const char *path) {
    if (path[0] == '/') return vfs_rmdir(path);
    return false;
}

bool fat32_delete(const char *path) {
    if (path[0] == '/') return vfs_delete(path);
    if (root_volume != NULL) {
        uint64_t rflags = spinlock_acquire_irqsave(&root_volume->lock);
        bool res = fat32_delete_vol(root_volume, path);
        spinlock_release_irqrestore(&root_volume->lock, rflags);
        return res;
    }
    return false;
}

int fat32_get_info(const char *path, fat32_file_info_t *info) {
    if (path[0] == '/') {
        vfs_dirent_t v_info;
        int res = vfs_get_info(path, &v_info);
        if (res == 0) {
            strcpy(info->name, v_info.name);
            info->size = v_info.size;
            info->is_directory = v_info.is_directory;
            info->start_cluster = v_info.start_cluster;
            info->write_date = v_info.write_date;
            info->write_time = v_info.write_time;
            return 0;
        }
        return -1;
    }

    if (root_volume != NULL) {
        uint64_t rflags = spinlock_acquire_irqsave(&root_volume->lock);
        fat32_file_handle_t *fh = fat32_open_vol(root_volume, path, "r");
        if (fh) {
            extract_filename(path, info->name);
            info->size = fh->size;
            info->start_cluster = fh->start_cluster;
            info->is_directory = fh->is_directory;
            fat32_close_nolock(fh);
            spinlock_release_irqrestore(&root_volume->lock, rflags);
            return 0;
        }
        spinlock_release_irqrestore(&root_volume->lock, rflags);
    }
    return -1;
}

bool fat32_exists(const char *path) {
    if (path[0] == '/') return vfs_exists(path);
    if (root_volume != NULL) {
        uint64_t rflags = spinlock_acquire_irqsave(&root_volume->lock);
        fat32_file_handle_t *fh = fat32_open_vol(root_volume, path, "r");
        if (fh) {
            fat32_close_nolock(fh);
            spinlock_release_irqrestore(&root_volume->lock, rflags);
            return true;
        }
        spinlock_release_irqrestore(&root_volume->lock, rflags);
    }
    return false;
}

bool fat32_rename(const char *old_path, const char *new_path) {
    (void)old_path;
    (void)new_path;
    return false;
}

bool fat32_is_directory_nolock(const char *path) {
    if (path[0] == '/') return vfs_is_directory(path);
    if (root_volume != NULL) {
        fat32_file_handle_t *fh = fat32_open_vol(root_volume, path, "r");
        if (fh) {
            bool is_dir = fh->is_directory;
            fat32_close_nolock(fh);
            return is_dir;
        }
    }
    return false;
}

bool fat32_is_directory(const char *path) {
    return fat32_is_directory_nolock(path);
}

int fat32_list_directory(const char *path, fat32_file_info_t *entries, int max_entries) {
    if (path[0] == '/') {
        vfs_dirent_t *v_entries = (vfs_dirent_t *)kmalloc(sizeof(vfs_dirent_t) * max_entries);
        if (!v_entries) return 0;
        
        int count = vfs_list_directory(path, v_entries, max_entries, 0);
        for (int i = 0; i < count; i++) {
            strcpy(entries[i].name, v_entries[i].name);
            entries[i].size = v_entries[i].size;
            entries[i].is_directory = v_entries[i].is_directory;
            entries[i].start_cluster = v_entries[i].start_cluster;
            entries[i].write_date = v_entries[i].write_date;
            entries[i].write_time = v_entries[i].write_time;
        }
        kfree_null(v_entries);
        return count;
    }
    if (root_volume != NULL) {
        uint64_t rflags = spinlock_acquire_irqsave(&root_volume->lock);
        int count = fat32_list_directory_vol(root_volume, path, entries, max_entries, 0);
        spinlock_release_irqrestore(&root_volume->lock, rflags);
        return count;
    }
    return 0;
}
