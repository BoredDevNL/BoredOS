// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "fat32.h"
#include "vfs.h"
#include "memory_manager.h"
#include "io.h"
#include "disk.h"
#include <stdbool.h>
#include <stddef.h>
#include "spinlock.h"

// Locks for FAT32 operations (SMP safety)
static spinlock_t ramfs_lock = SPINLOCK_INIT; // Protects the RAM-based filesystem (/)


#define MAX_CLUSTERS 32768
#define MAX_OPEN_HANDLES 32

// In-memory FAT table
static uint32_t fat_table[MAX_CLUSTERS];
static uint8_t cluster_data[MAX_CLUSTERS][FAT32_CLUSTER_SIZE];

typedef struct FileEntry {
    char full_path[FAT32_MAX_PATH];
    char filename[FAT32_MAX_FILENAME];
    uint32_t start_cluster;
    uint32_t size;
    uint32_t attributes;
    bool used;            
    char parent_path[FAT32_MAX_PATH];
    struct FileEntry *next; 
} FileEntry;

static FileEntry *file_list_head = NULL; 
static uint32_t next_cluster = 3;  // Start after reserved clusters 0, 1, 2
static FAT32_FileHandle open_handles[MAX_OPEN_HANDLES];
static char current_dir[FAT32_MAX_PATH] = "/";
static char current_drive = 'A';  // Backward compat only
static int desktop_file_limit = -1;

// === RealFS Definitions ===

typedef struct {
    Disk *disk;
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t fat_size; // sectors
    uint32_t num_fats;
    uint32_t total_sectors;
    uint32_t partition_offset; // LBA offset of partition start
    bool mounted;
    uint32_t cached_fat_sector;
    uint8_t cached_fat_buf[512];
    uint32_t last_allocated_cluster; // Hint for faster allocation
    spinlock_t lock; // Per-volume lock for physical disk operations
} FAT32_Volume;

// Dynamically allocated volumes (no longer A-Z indexed)
static bool realfs_mkdir_vol(FAT32_Volume *vol, const char *path);
extern void serial_write(const char *str);
extern void serial_write_hex(uint32_t val);
#define MAX_REAL_VOLUMES 8
static FAT32_Volume *real_volumes[MAX_REAL_VOLUMES];
static int real_volume_count = 0;
static FAT32_Volume *root_volume = NULL;

// Forward declarations for volume-aware functions
static uint32_t realfs_allocate_cluster(FAT32_Volume *vol);
static void handle_fat32_truncate(FAT32_FileHandle *handle);
static FAT32_FileHandle* realfs_open_from_vol(FAT32_Volume *vol, const char *path, const char *mode);
static int realfs_list_directory_vol(FAT32_Volume *vol, const char *path, FAT32_FileInfo *entries, int max_entries, int offset);
static bool realfs_delete_from_vol(FAT32_Volume *vol, const char *path);
static bool realfs_mount_volume(FAT32_Volume *vol, Disk *disk);
static void realfs_update_dir_entry_size(FAT32_Volume *vol, FAT32_FileHandle *handle);
static uint32_t realfs_allocate_cluster(FAT32_Volume *vol);
static int realfs_read_cluster(FAT32_Volume *vol, uint32_t cluster, uint8_t *buffer);
static int realfs_write_cluster(FAT32_Volume *vol, uint32_t cluster, const uint8_t *buffer);
static uint32_t realfs_next_cluster(FAT32_Volume *vol, uint32_t cluster);
static bool realfs_find_contiguous_free(FAT32_Volume *vol, uint32_t dir_start_cluster, int n, uint32_t *out_cluster, int *out_entry_idx);
static uint8_t fat_lfn_checksum(const uint8_t *short_name);
static void extract_lfn_chars(FAT32_LFNEntry *lfn, char *buffer);
static void to_dos_filename(const char *filename, char *dos_name);
static bool realfs_create_entry(FAT32_Volume *vol, uint32_t parent_cluster, const char *name, uint8_t attributes, uint32_t start_cluster, uint32_t file_size, uint32_t *out_sector, uint32_t *out_offset);

void fat32_set_root_volume(void *fs_private) {
    root_volume = (FAT32_Volume*)fs_private;
}

static void fat32_sync_if_root(FAT32_Volume *vol) {
    if (!vol || vol != root_volume) return;
    disk_sync(vol->disk);
}

// === Helper Functions (Shared) ===

// Serial debug output
static void fs_serial_char(char c) {
    while (!(inb(0x3F8 + 5) & 0x20));
    outb(0x3F8, c);
}
static void fs_serial_str(const char *s) { while (*s) fs_serial_char(*s++); }
static void fs_serial_num(uint32_t n) {
    if (n >= 10) fs_serial_num(n / 10);
    fs_serial_char('0' + (n % 10));
}

static size_t fs_strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

static void fs_strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}

static int fs_strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

static void fs_strcat(char *dest, const char *src) {
    while (*dest) dest++;
    fs_strcpy(dest, src);
}


bool fs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*prefix++ != *str++) return false;
    }
    return true;
}

// Extract filename from path
static void extract_filename(const char *path, char *filename) {
    int len = fs_strlen(path);
    int i = len - 1;
    while (i > 0 && path[i] == '/') i--;
    int start = i;
    while (start >= 0 && path[start] != '/') start--;
    start++;
    int j = 0;
    for (int k = start; k <= i; k++) {
        filename[j++] = path[k];
    }
    filename[j] = 0;
}

// Extract parent path
static void extract_parent_path(const char *path, char *parent) {
    int len = fs_strlen(path);
    int i = len - 1;
    while (i > 0 && path[i] == '/') i--;
    while (i > 0 && path[i] != '/') i--;
    if (i <= 0) {
        parent[0] = '/';
        parent[1] = 0;
    } else {
        for (int j = 0; j < i; j++) {
            parent[j] = path[j];
        }
        parent[i] = 0;
    }
}

// Helper to parse drive from path (backward compat)
static char parse_drive_from_path(const char **path_ptr) {
    const char *path = *path_ptr;
    if (path[0] && path[1] == ':') {
        char drive = path[0];
        if (drive >= 'a' && drive <= 'z') drive -= 32; // toupper
        *path_ptr = path + 2;
        return drive;
    }

    if (root_volume != NULL) return 0; // Pseudo-drive 0 means "use root volume"
    return 'A'; // Default to RAMFS
}


// Normalize path (remove .., ., etc)
void fat32_normalize_path(const char *path, char *normalized) {
    char *temp = (char*)kmalloc(FAT32_MAX_PATH);
    if (!temp) {
        if (normalized) normalized[0] = 0;
        return;
    }
    int temp_len = 0;
    const char *p = path;
    // Strip drive letter if present (backward compat)
    if (p[0] && p[1] == ':') p += 2;
    (void)current_drive;
    

    if (p[0] == '/') {
        fs_strcpy(temp, "/");
        temp_len = 1;
    } else {
        fs_strcpy(temp, current_dir);
        temp_len = fs_strlen(temp);
    }

    int i = 0;
    while (p[i]) {
        while (p[i] == '/') i++;
        if (!p[i]) break;
        char component[FAT32_MAX_FILENAME];
        int j = 0;
        while (p[i] && p[i] != '/' && j < (FAT32_MAX_FILENAME - 1)) {
            component[j++] = p[i++];
        }
        component[j] = 0;

        if (fs_strcmp(component, ".") == 0) {
            continue;
        } else if (fs_strcmp(component, "..") == 0) {
            if (temp_len > 1) {
                while (temp_len > 0 && temp[temp_len - 1] != '/') temp_len--;
                if (temp_len > 1) temp_len--;
                temp[temp_len] = 0;
            }
        } else {
            int comp_len = fs_strlen(component);
            if (temp_len + comp_len + 2 < FAT32_MAX_PATH) {
                if (temp[temp_len - 1] != '/') {
                    temp[temp_len++] = '/';
                    temp[temp_len] = 0;
                }
                fs_strcat(temp, component);
                temp_len = fs_strlen(temp);
            }
        }
    }
    if (temp_len > 1 && temp[temp_len - 1] == '/') temp[--temp_len] = 0;
    fs_strcpy(normalized, temp);
    kfree(temp);
}

// === RAMFS Internal Functions ===

static FileEntry* ramfs_find_file(const char *path) {
    char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
    if (!normalized) return NULL;
    fat32_normalize_path(path, normalized);
    FileEntry *ret = NULL;
    for (FileEntry *n = file_list_head; n; n = n->next) {
        if (fs_strcmp(n->full_path, normalized) == 0) {
            ret = n;
            break;
        }
    }
    kfree(normalized);
    return ret;
}

static FileEntry* ramfs_alloc_entry(void) {
    FileEntry *e = (FileEntry*)kmalloc(sizeof(FileEntry));
    if (!e) return NULL;
    for (int i = 0; i < (int)sizeof(FileEntry); i++) ((char*)e)[i] = 0;
    e->used = true;
    e->next = file_list_head;
    file_list_head = e;
    return e;
}

static void ramfs_free_cluster_chain(uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    uint32_t guard = 0;

    while (cluster >= 3 && cluster < MAX_CLUSTERS && fat_table[cluster] != 0 && guard++ < MAX_CLUSTERS) {
        uint32_t next = fat_table[cluster];
        fat_table[cluster] = 0;
        if (next >= 0xFFFFFFF8 || next == cluster) break;
        cluster = next;
    }
}

static void ramfs_free_entry(FileEntry *entry) {
    if (!entry) return;
    ramfs_free_cluster_chain(entry->start_cluster);
    if (file_list_head == entry) {
        file_list_head = entry->next;
    } else {
        for (FileEntry *n = file_list_head; n && n->next; n = n->next) {
            if (n->next == entry) {
                n->next = entry->next;
                break;
            }
        }
    }
    kfree(entry);
}

static FAT32_FileHandle* ramfs_find_free_handle(void) {
    for (int i = 0; i < MAX_OPEN_HANDLES; i++) {
        if (!open_handles[i].valid) return &open_handles[i];
    }
    return NULL;
}

static uint32_t ramfs_allocate_cluster(void) {
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = pass == 0 ? next_cluster : 3;
        uint32_t end = pass == 0 ? MAX_CLUSTERS : next_cluster;

        for (uint32_t cluster = start; cluster < end; cluster++) {
            if (fat_table[cluster] == 0) {
                fat_table[cluster] = 0xFFFFFFFF;
                next_cluster = cluster + 1;
                if (next_cluster >= MAX_CLUSTERS) next_cluster = 3;
                for (int i = 0; i < FAT32_CLUSTER_SIZE; i++) {
                    cluster_data[cluster][i] = 0;
                }
                return cluster;
            }
        }
    }

    return 0;
}

static int ramfs_count_files_in_dir(const char *normalized_path) {
    int count = 0;
    for (FileEntry *n = file_list_head; n; n = n->next) {
        if (fs_strcmp(n->parent_path, normalized_path) == 0) count++;
    }
    return count;
}

static bool check_desktop_limit(const char *normalized_path) {
    if (desktop_file_limit < 0) return true;
    if (fs_strlen(normalized_path) > 14 && 
        normalized_path[0] == '/' && 
        normalized_path[1] == 'r' && normalized_path[2] == 'o' && 
        normalized_path[3] == 'o' && normalized_path[4] == 't' && 
        normalized_path[5] == '/' &&
        normalized_path[6] == 'D' && normalized_path[7] == 'e' && 
        normalized_path[8] == 's' && normalized_path[9] == 'k' && 
        normalized_path[10] == 't' && normalized_path[11] == 'o' && 
        normalized_path[12] == 'p' && normalized_path[13] == '/') {
        const char *p = normalized_path + 14;
        while (*p) {
            if (*p == '/') return true; 
            p++;
        }
        
        int count = ramfs_count_files_in_dir("/root/Desktop");
        if (count >= desktop_file_limit) return false;
    }
    return true;
}

static FAT32_FileHandle* ramfs_open(const char *normalized_path, const char *mode) {
    FileEntry *entry = ramfs_find_file(normalized_path);
    
    if (mode[0] == 'r') {
        if (!entry || (entry->attributes & ATTR_DIRECTORY)) return NULL;
    } else if (mode[0] == 'w' || (mode[0] == 'a')) {
        if (!entry) {
            if (!check_desktop_limit(normalized_path)) return NULL;
            entry = ramfs_alloc_entry();
            if (!entry) return NULL;
            fs_strcpy(entry->full_path, normalized_path);
            extract_filename(normalized_path, entry->filename);
            extract_parent_path(normalized_path, entry->parent_path);
            entry->start_cluster = ramfs_allocate_cluster();
            if (!entry->start_cluster) { ramfs_free_entry(entry); return NULL; }
            entry->size = 0;
            entry->attributes = 0;
        }
        if (mode[0] == 'w') {
            ramfs_free_cluster_chain(entry->start_cluster);
            entry->start_cluster = ramfs_allocate_cluster();
            if (!entry->start_cluster) return NULL;
            entry->size = 0;
        }
    }
    
    FAT32_FileHandle *handle = ramfs_find_free_handle();
    if (!handle) return NULL;
    
    handle->valid = true;
    handle->volume = NULL;  // RAMFS handle
    handle->cluster = entry->start_cluster;
    handle->start_cluster = entry->start_cluster;
    handle->position = 0;
    handle->size = entry->size;
    handle->is_directory = (entry->attributes & ATTR_DIRECTORY) != 0;
    handle->attributes = entry->attributes;
    
    if (mode[0] == 'r') handle->mode = 0;
    else if (mode[0] == 'w') handle->mode = 1;
    else {
        handle->mode = 2;
        handle->position = entry->size;
        uint32_t current_cluster = handle->start_cluster;
        uint32_t pos = 0;
        while (pos + FAT32_CLUSTER_SIZE <= handle->position) {
             uint32_t next = fat_table[current_cluster];
             if (next >= 0xFFFFFFF8) break; 
             current_cluster = next;
             pos += FAT32_CLUSTER_SIZE;
        }
        handle->cluster = current_cluster;
    }
    return handle;
}

static int ramfs_read(FAT32_FileHandle *handle, void *buffer, int size) {
    int bytes_read = 0;
    uint8_t *buf = (uint8_t *)buffer;
    
    while (bytes_read < size && handle->position < handle->size) {
        uint32_t offset_in_cluster = handle->position % FAT32_CLUSTER_SIZE;
        int to_read = size - bytes_read;
        int available = handle->size - handle->position;
        if (to_read > available) to_read = available;
        if (to_read > (int)(FAT32_CLUSTER_SIZE - offset_in_cluster)) to_read = FAT32_CLUSTER_SIZE - offset_in_cluster;
        
        if (handle->cluster >= MAX_CLUSTERS) break;
        
        uint8_t *src = cluster_data[handle->cluster] + offset_in_cluster;
        for (int i = 0; i < to_read; i++) buf[bytes_read + i] = src[i];
        
        bytes_read += to_read;
        handle->position += to_read;
        if (handle->position % FAT32_CLUSTER_SIZE == 0 && handle->position < handle->size) {
            handle->cluster = fat_table[handle->cluster];
        }
    }
    return bytes_read;
}

static int ramfs_write(FAT32_FileHandle *handle, const void *buffer, int size) {
    int bytes_written = 0;
    const uint8_t *buf = (const uint8_t *)buffer;
    
    if (handle->position > 0 && (handle->position % FAT32_CLUSTER_SIZE) == 0) {
         uint32_t next = fat_table[handle->cluster];
         if (next >= 0xFFFFFFF8) {
             next = ramfs_allocate_cluster();
             if (!next) return 0;
             fat_table[handle->cluster] = next;
         }
         handle->cluster = next;
    }

    while (bytes_written < size) {
        uint32_t offset_in_cluster = handle->position % FAT32_CLUSTER_SIZE;
        int to_write = size - bytes_written;
        if (to_write > (int)(FAT32_CLUSTER_SIZE - offset_in_cluster)) to_write = FAT32_CLUSTER_SIZE - offset_in_cluster;
        
        if (handle->cluster >= MAX_CLUSTERS) break;
        
        uint8_t *dest = cluster_data[handle->cluster] + offset_in_cluster;
        for (int i = 0; i < to_write; i++) dest[i] = buf[bytes_written + i];
        
        bytes_written += to_write;
        handle->position += to_write;
        if (handle->position > handle->size) handle->size = handle->position;
        
        if (offset_in_cluster + to_write >= FAT32_CLUSTER_SIZE && bytes_written < size) {
            uint32_t next = ramfs_allocate_cluster();
            if (!next) break;
            fat_table[handle->cluster] = next;
            handle->cluster = next;
        }
    }
    
    for (FileEntry *n = file_list_head; n; n = n->next) {
        if (n->start_cluster == handle->start_cluster) {
            n->size = handle->size;
            break;
        }
    }
    return bytes_written;
}

// === RealFS Implementation ===

static bool realfs_mount_volume(FAT32_Volume *vol, Disk *disk) {
    if (vol->mounted) return true;
    
    // The disk object abstractions handles the partition offset internally.
    // LBA 0 of 'disk' is the FAT32 boot sector.
    uint32_t part_offset = 0;
    
    uint8_t *sect0 = (uint8_t*)kmalloc(512);
    if (!sect0) return false;
    
    // Read BPB from partition start
    if (disk->read_sector(disk, part_offset, sect0) != 0) {
        kfree(sect0);
        return false;
    }
    
    FAT32_BootSector *bpb = (FAT32_BootSector*)sect0;
    
    if (bpb->boot_signature_value != 0xAA55) {
        kfree(sect0);
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
    
    fs_serial_str("[FAT32] Mounted volume: /dev/");
    fs_serial_str(disk->devname);
    fs_serial_str(" part_offset=");
    fs_serial_num(part_offset);
    fs_serial_str(" fat_lba=");
    fs_serial_num(vol->fat_begin_lba);
    fs_serial_str(" cluster_lba=");
    fs_serial_num(vol->cluster_begin_lba);
    fs_serial_str(" spc=");
    fs_serial_num(vol->sectors_per_cluster);
    fs_serial_str(" root_cl=");
    fs_serial_num(vol->root_cluster);
    fs_serial_str("\n");
    
    kfree(sect0);
    return true;
}

static uint32_t realfs_next_cluster(FAT32_Volume *vol, uint32_t cluster);
static void realfs_update_dir_entry_size(FAT32_Volume *vol, FAT32_FileHandle *handle) {
    if (handle->is_directory) return;
    if (handle->dir_sector != 0 && handle->dir_offset != 0xFFFFFFFF && handle->dir_offset < 512) {
        uint8_t *dir_buf = (uint8_t*)kmalloc(512);
        if (dir_buf && vol->disk->read_sector(vol->disk, handle->dir_sector, dir_buf) == 0) {
            FAT32_DirEntry *entry = (FAT32_DirEntry*)(dir_buf + handle->dir_offset);
            // Update start cluster if it exists
            if (handle->start_cluster != 0) {
                entry->start_cluster_high = (handle->start_cluster >> 16);
                entry->start_cluster_low = (handle->start_cluster & 0xFFFF);
            }
            // Always update file size
            entry->file_size = handle->size;
            // Write back
            vol->disk->write_sector(vol->disk, handle->dir_sector, dir_buf);
        }
        if (dir_buf) kfree(dir_buf);
    }
}

static uint32_t realfs_next_cluster(FAT32_Volume *vol, uint32_t cluster) {
    uint32_t fat_sector = vol->fat_begin_lba + (cluster * 4) / 512;
    uint32_t fat_offset = (cluster * 4) % 512;
    
    if (vol->cached_fat_sector != fat_sector) {
        if (vol->disk->read_sector(vol->disk, fat_sector, vol->cached_fat_buf) != 0) {
            return 0xFFFFFFFF;
        }
        vol->cached_fat_sector = fat_sector;
    }
    
    uint32_t next = *(uint32_t*)&vol->cached_fat_buf[fat_offset];
    next &= 0x0FFFFFFF; // Mask top 4 bits
    
    // Safety against infinite loops on corrupted or unallocated FAT chains
    if (next == 0 || next == cluster) return 0x0FFFFFFF;
    
    return next;
}

static void realfs_set_fat_entry(FAT32_Volume *vol, uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint32_t sector_offset = offset / 512;
    uint32_t byte_offset = offset % 512;

    uint8_t *buf = (uint8_t*)kmalloc(512);
    if (!buf) return;

    for (uint32_t i = 0; i < vol->num_fats; i++) {
        uint32_t sector = vol->fat_begin_lba + (i * vol->fat_size) + sector_offset;
        if (vol->disk->read_sector(vol->disk, sector, buf) == 0) {
            uint32_t *entry = (uint32_t*)(buf + byte_offset);
            *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
            vol->disk->write_sector(vol->disk, sector, buf);
            if (vol->cached_fat_sector == sector) vol->cached_fat_sector = 0xFFFFFFFF;
        }
    }
    kfree(buf);
}

static int realfs_read_cluster(FAT32_Volume *vol, uint32_t cluster, uint8_t *buffer) {
    if (!vol || cluster < 2) return -1;
    uint32_t lba = vol->cluster_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
    if (vol->disk->read_sectors) {
        return vol->disk->read_sectors(vol->disk, lba, vol->sectors_per_cluster, buffer);
    }
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        if (vol->disk->read_sector(vol->disk, lba + i, buffer + (i * 512)) != 0) return -1;
    }
    return 0;
}

static void to_dos_filename(const char *filename, char *out) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int len = fs_strlen(filename);
    int dot = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (filename[i] == '.') { dot = i; break; }
    }
    int name_len = (dot == -1) ? len : dot;
    if (name_len > 8) name_len = 8;
    for (int i = 0; i < name_len; i++) {
        char c = filename[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    if (dot != -1) {
        int ext_len = len - dot - 1;
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++) {
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

static bool realfs_create_entry(FAT32_Volume *vol, uint32_t parent_cluster, const char *name, uint8_t attributes, uint32_t start_cluster, uint32_t file_size, uint32_t *out_sector, uint32_t *out_offset) {
    char dos_name[11];
    to_dos_filename(name, dos_name);
    
    int name_len = fs_strlen(name);
    bool needs_lfn = false;
    
    int dot_pos = -1;
    for (int i = 0; i < name_len; i++) {
        if (name[i] == '.') { dot_pos = i; break; }
        if (name[i] >= 'a' && name[i] <= 'z') needs_lfn = true;
    }
    if (!needs_lfn) {
        if (dot_pos == -1) needs_lfn = (name_len > 8);
        else needs_lfn = (dot_pos > 8) || (name_len - dot_pos - 1 > 3);
    }
    if (!needs_lfn && dot_pos != -1) {
        for (int i = dot_pos + 1; i < name_len; i++) {
            if (name[i] >= 'a' && name[i] <= 'z') { needs_lfn = true; break; }
        }
    }

    int lfn_entries = needs_lfn ? ((name_len + 12) / 13) : 0;
    int total_entries = lfn_entries + 1;
    
    uint32_t free_cluster = 0;
    int start_idx = -1;
    
    if (!realfs_find_contiguous_free(vol, parent_cluster, total_entries, &free_cluster, &start_idx)) {
        return false;
    }
    
    uint8_t *buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
    if (!buf) return false;
    
    if (realfs_read_cluster(vol, free_cluster, buf) != 0) {
        kfree(buf);
        return false;
    }
    
    FAT32_DirEntry *entries = (FAT32_DirEntry*)buf;
    uint8_t checksum = fat_lfn_checksum((uint8_t*)dos_name);
    
    for (int i = 0; i < lfn_entries; i++) {
        FAT32_LFNEntry *lfn = (FAT32_LFNEntry*)&entries[start_idx + i];
        lfn->order = (lfn_entries - i);
        if (i == 0) lfn->order |= 0x40;
        lfn->attr = ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->first_cluster = 0;
        
        int char_offset = (lfn_entries - i - 1) * 13;
        for (int k = 0; k < 13; k++) {
            uint16_t c = 0xFFFF;
            if (char_offset + k < name_len) c = name[char_offset + k];
            else if (char_offset + k == name_len) c = 0x0000;
            
            if (k < 5) lfn->name1[k] = c;
            else if (k < 11) lfn->name2[k-5] = c;
            else lfn->name3[k-11] = c;
        }
    }
    
    FAT32_DirEntry *d = &entries[start_idx + lfn_entries];
    for (int k = 0; k < 8; k++) d->filename[k] = dos_name[k];
    for (int k = 0; k < 3; k++) d->extension[k] = dos_name[8+k];
    d->attributes = attributes;
    d->start_cluster_high = (start_cluster >> 16);
    d->start_cluster_low = (start_cluster & 0xFFFF);
    d->file_size = file_size;
    
    if (realfs_write_cluster(vol, free_cluster, buf) != 0) {
        kfree(buf);
        return false;
    }
    
    uint32_t lba = vol->cluster_begin_lba + (free_cluster - 2) * vol->sectors_per_cluster;
    *out_sector = lba + ((start_idx + lfn_entries) * 32) / 512;
    *out_offset = ((start_idx + lfn_entries) * 32) % 512;
    
    kfree(buf);
    return true;
}

static FAT32_FileHandle* realfs_open_from_vol(FAT32_Volume *vol, const char *path, const char *mode) {
    if (!vol || !vol->mounted) return NULL;
    
    // Parse path to find start cluster
    uint32_t current_cluster = vol->root_cluster;
    
    // Skip leading slash
    const char *p = path;
    if (*p == '/') p++;
    

    
    if (*p == 0) {
        // Root dir
        if (mode[0] == 'w') return NULL; // Cannot write to root as file
        FAT32_FileHandle *fh = ramfs_find_free_handle(); // Reuse handle pool
        if (fh) {
            fh->valid = true;
            fh->volume = vol;
            fh->start_cluster = vol->root_cluster;
            fh->cluster = vol->root_cluster;
            fh->position = 0;
            fh->size = 0; // Unknown for root
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
    
    uint8_t *cluster_buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return NULL;
    uint8_t *lfn_cl_buf = NULL;

    while (*p) {
        // Extract component
        int i = 0;
        while (*p && *p != '/') {
            component[i++] = *p++;
        }
        component[i] = 0;
        if (*p == '/') p++; // Skip separator
        
        // Search in current_cluster
        found = false;
        uint32_t search_cluster = current_cluster;
        
        char lfn_buffer[256];
        bool has_lfn = false;
        for(int i=0; i<256; i++) lfn_buffer[i] = 0;

        while (search_cluster < 0x0FFFFFF8) {
            if (realfs_read_cluster(vol, search_cluster, cluster_buf) != 0) break;
            
            FAT32_DirEntry *entry = (FAT32_DirEntry*)cluster_buf;
            int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
            
            for (int e = 0; e < entries_per_cluster; e++) {
                if (entry[e].filename[0] == 0) break; // End of dir
                
                if (entry[e].filename[0] == 0xE5) {
                    has_lfn = false;
                    continue; // Deleted
                }
                
                if (entry[e].attributes == ATTR_LFN) {
                    FAT32_LFNEntry *lfn = (FAT32_LFNEntry*)&entry[e];
                    if (lfn->order & 0x40) {
                        for(int i=0; i<256; i++) lfn_buffer[i] = 0;
                    }
                    extract_lfn_chars(lfn, lfn_buffer);
                    has_lfn = true;
                    continue;
                }
                
                if (entry[e].attributes & ATTR_VOLUME_ID) {
                    has_lfn = false;
                    continue; // Volume label
                }
                
                // Compare name
                char name[256];
                if (has_lfn && lfn_buffer[0] != 0) {
                    fs_strcpy(name, lfn_buffer);
                    has_lfn = false;
                } else {
                    int n = 0;
                    for (int k = 0; k < 8 && entry[e].filename[k] != ' '; k++) name[n++] = entry[e].filename[k];
                    if (entry[e].extension[0] != ' ') {
                        name[n++] = '.';
                        for (int k = 0; k < 3 && entry[e].extension[k] != ' '; k++) name[n++] = entry[e].extension[k];
                    }
                    name[n] = 0;
                }
                
                // Case insensitive compare
                bool match = true;
                int clen = fs_strlen(component);
                int nlen = fs_strlen(name);
                if (clen != nlen) match = false;
                else {
                    for (int c = 0; c < clen; c++) {
                        char c1 = name[c];
                        char c2 = component[c];
                        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                        if (c1 != c2) { match = false; break; }
                    }
                }
                
                if (match) {
                    uint32_t cluster = (entry[e].start_cluster_high << 16) | entry[e].start_cluster_low;
                    
                    uint32_t lba = vol->cluster_begin_lba + (search_cluster - 2) * vol->sectors_per_cluster;
                    int sect_in_cluster = (e * 32) / 512;
                    entry_sector = lba + sect_in_cluster;
                    entry_offset = (e * 32) % 512;

                    if (*p == 0) {
                        // Found target
                        current_cluster = cluster;
                        file_size = entry[e].file_size;
                        attributes = entry[e].attributes;
                        is_directory = (attributes & ATTR_DIRECTORY) != 0;
                        found = true;
                    } else {
                        // It must be a directory
                        if (entry[e].attributes & ATTR_DIRECTORY) {
                            current_cluster = cluster;
                            found = true;
                        }
                    }
                    break;
                }
            }
            if (found) break;
            search_cluster = realfs_next_cluster(vol, search_cluster);
        }
        
        if (!found) {
            if ((mode[0] == 'w' || mode[0] == 'a') && *p == 0) {
                   if (realfs_create_entry(vol, current_cluster, component, ATTR_ARCHIVE, 0, 0, &entry_sector, &entry_offset)) {
                       FAT32_FileHandle *fh = ramfs_find_free_handle();
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
                           if (cluster_buf) kfree(cluster_buf);
                           return fh;
                       }
                   }
                   if (cluster_buf) kfree(cluster_buf);
                   return NULL; 
            }
            if (cluster_buf) { kfree(cluster_buf); cluster_buf = NULL; }
            return NULL;

        }
    }

    
    // Found file/dir
    FAT32_FileHandle *fh = ramfs_find_free_handle();
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
            handle_fat32_truncate(fh);
        }
        
        if (mode[0] == 'a') {
            fh->position = fh->size;
            // Seek to EOF cluster
            uint32_t cluster_size = vol->sectors_per_cluster * 512;
            uint32_t pos = 0;
            while (pos + cluster_size <= fh->position) {
                uint32_t next = realfs_next_cluster(vol, fh->cluster);
                if (next >= 0x0FFFFFF8) break;
                fh->cluster = next;
                pos += cluster_size;
            }
        }
    }
    if (cluster_buf) kfree(cluster_buf);
    if (lfn_cl_buf) kfree(lfn_cl_buf);
    return fh;
}

static int realfs_read_file(FAT32_FileHandle *handle, void *buffer, int size, uint8_t *ext_cluster_buf) {
    FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
    if (!vol) return 0;
    
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = ext_cluster_buf;
    bool free_buf = false;
    
    if (!cluster_buf) {
        cluster_buf = (uint8_t*)kmalloc(cluster_size);
        if (!cluster_buf) return 0;
        free_buf = true;
    }
    
    int bytes_read = 0;
    uint8_t *out_buf = (uint8_t*)buffer;
    
    while (bytes_read < size && handle->position < handle->size) {
        if (realfs_read_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        
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
            handle->cluster = realfs_next_cluster(vol, handle->cluster);
            if (handle->cluster >= 0x0FFFFFF8) break;
        }
    }
    
    if (free_buf) kfree(cluster_buf);
    return bytes_read;
}

static int realfs_write_cluster(FAT32_Volume *vol, uint32_t cluster, const uint8_t *buffer) {
    if (!vol || cluster < 2 || cluster >= 0x0FFFFFF8) return -1;
    uint32_t lba = vol->cluster_begin_lba + (cluster - 2) * vol->sectors_per_cluster;
    if (vol->disk->write_sectors) {
        return vol->disk->write_sectors(vol->disk, lba, vol->sectors_per_cluster, buffer);
    }
    for (uint32_t i = 0; i < vol->sectors_per_cluster; i++) {
        if (vol->disk->write_sector(vol->disk, lba + i, buffer + (i * 512)) != 0) return -1;
    }
    return 0;
}

// Helper to find N contiguous free entries in a directory cluster chain
static bool realfs_find_contiguous_free(FAT32_Volume *vol, uint32_t dir_start_cluster, int n, uint32_t *out_cluster, int *out_entry_idx) {
    uint32_t current = dir_start_cluster;
    uint8_t *cluster_buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return false;
    
    int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
    
    while (current < 0x0FFFFFF8) {
        if (realfs_read_cluster(vol, current, cluster_buf) != 0) break;
        FAT32_DirEntry *entries = (FAT32_DirEntry*)cluster_buf;
        
        int contiguous = 0;
        int start_idx = -1;
        
        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].filename[0] == 0 || entries[i].filename[0] == 0xE5) {
                if (contiguous == 0) start_idx = i;
                contiguous++;
                if (contiguous >= n) {
                    *out_cluster = current;
                    *out_entry_idx = start_idx;
                    kfree(cluster_buf);
                    return true;
                }
            } else {
                contiguous = 0;
            }
        }
        
        uint32_t next = realfs_next_cluster(vol, current);
        if (next >= 0x0FFFFFF8) {
            // No more clusters, try to allocate one more for the directory
            uint32_t new_cluster = realfs_allocate_cluster(vol);
            if (new_cluster != 0) {
                // Link last cluster to new cluster
                uint32_t fat_sector = vol->fat_begin_lba + (current * 4) / 512;
                uint32_t fat_offset = (current * 4) % 512;
                uint8_t *fat_buf = (uint8_t*)kmalloc(512);
                if (fat_buf && vol->disk->read_sector(vol->disk, fat_sector, fat_buf) == 0) {
                    *(uint32_t*)&fat_buf[fat_offset] = (new_cluster & 0x0FFFFFFF);
                    vol->disk->write_sector(vol->disk, fat_sector, fat_buf);
                }
                if (fat_buf) kfree(fat_buf);
                
                // Zero new cluster
                uint8_t *zero_buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
                if (zero_buf) {
                    for(uint32_t k=0; k<vol->sectors_per_cluster*512; k++) zero_buf[k] = 0;
                    realfs_write_cluster(vol, new_cluster, zero_buf);
                    kfree(zero_buf);
                }
                
                // Mark as EOF in FAT
                fat_sector = vol->fat_begin_lba + (new_cluster * 4) / 512;
                fat_offset = (new_cluster * 4) % 512;
                fat_buf = (uint8_t*)kmalloc(512);
                if (fat_buf && vol->disk->read_sector(vol->disk, fat_sector, fat_buf) == 0) {
                    *(uint32_t*)&fat_buf[fat_offset] = 0x0FFFFFF8;
                    vol->disk->write_sector(vol->disk, fat_sector, fat_buf);
                }
                if (fat_buf) kfree(fat_buf);
                
                next = new_cluster;
            }
        }
        current = next;
    }
    
    kfree(cluster_buf);
    return false;
}

static uint32_t realfs_allocate_cluster(FAT32_Volume *vol) {
    uint32_t current = vol->last_allocated_cluster;
    if (current < 3) current = 3;
    
    uint32_t fat_entries = (vol->fat_size * 512) / 4;
    uint32_t first_search = current;
    
    uint8_t *fat_buf = (uint8_t*)kmalloc(512);
    if (!fat_buf) return 0;
    
    uint32_t cached_sector = 0xFFFFFFFF;
    
    while (current < fat_entries) {
        uint32_t sector = vol->fat_begin_lba + (current * 4) / 512;
        uint32_t offset = (current * 4) % 512;
        
        if (sector != cached_sector) {
            vol->disk->read_sector(vol->disk, sector, fat_buf);
            cached_sector = sector;
        }
        
        uint32_t val = *(uint32_t*)&fat_buf[offset];
        if ((val & 0x0FFFFFFF) == 0) {
            kfree(fat_buf);
            realfs_set_fat_entry(vol, current, 0x0FFFFFFF); // EOC
            vol->last_allocated_cluster = current;
            return current;
        }
        current++;
        if (current >= fat_entries) current = 2;
        if (current == first_search) break; 
    }
    kfree(fat_buf);
    return 0; // Full
}

static void realfs_free_cluster_chain(FAT32_Volume *vol, uint32_t start_cluster) {
    if (start_cluster == 0 || start_cluster >= 0x0FFFFFF8) return;
    
    uint32_t current = start_cluster;
    while (current < 0x0FFFFFF8 && current >= 2) {
        uint32_t next = realfs_next_cluster(vol, current);
        realfs_set_fat_entry(vol, current, 0);
        if (next == current) break; 
        current = next;
    }
}

static void handle_fat32_truncate(FAT32_FileHandle *handle) {
    FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
    if (!vol || handle->start_cluster == 0) return;
    
    uint32_t start = handle->start_cluster;
    handle->start_cluster = 0;
    handle->cluster = 0;
    handle->size = 0;
    handle->position = 0;
    
    realfs_free_cluster_chain(vol, start);
    realfs_update_dir_entry_size(vol, handle);
}

static int realfs_write_file(FAT32_FileHandle *handle, const void *buffer, int size, uint8_t *ext_cluster_buf) {
    FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
    if (!vol) return 0;
    
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = ext_cluster_buf;
    bool free_buf = false;
    
    if (!cluster_buf) {
        cluster_buf = (uint8_t*)kmalloc(cluster_size);
        if (!cluster_buf) return 0;
        free_buf = true;
    }
    
    int bytes_written = 0;
    const uint8_t *src_buf = (const uint8_t*)buffer;
    
    if (handle->cluster == 0) {
        uint32_t new_cluster = realfs_allocate_cluster(vol);
        if (new_cluster == 0) {
            if (free_buf) kfree(cluster_buf);
            return 0;
        }
        handle->start_cluster = new_cluster;
        handle->cluster = new_cluster;
        handle->position = 0;
        handle->size = 0;
        
        // Update directory entry immediately with correct start_cluster
        // This ensures the directory always points to the right cluster
        realfs_update_dir_entry_size(vol, handle);
    }
    
    while (bytes_written < size) {
        uint32_t offset = handle->position % cluster_size;

        if (offset == 0 && handle->position > 0) {
            uint32_t next = realfs_next_cluster(vol, handle->cluster);
            if (next >= 0x0FFFFFF8) {
                uint32_t new_cluster = realfs_allocate_cluster(vol);
                if (new_cluster == 0) break;

                realfs_set_fat_entry(vol, handle->cluster, new_cluster);
                next = new_cluster;
            }
            handle->cluster = next;
        }

        int to_copy = size - bytes_written;
        int available = cluster_size - offset; 
        if (to_copy > available) to_copy = available;


        if (offset > 0 || (handle->position < handle->size && (handle->position + to_copy) < handle->size)) {
            if (realfs_read_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        } else {
            if (to_copy < (int)cluster_size) {
                for (int i = 0; i < (int)cluster_size; i++) cluster_buf[i] = 0;
            }
        }
        
        // Copy new data into the cluster buffer
        for (int i = 0; i < to_copy; i++) {
            cluster_buf[offset + i] = src_buf[bytes_written + i];
        }
        
        if (realfs_write_cluster(vol, handle->cluster, cluster_buf) != 0) break;
        
        bytes_written += to_copy;
        handle->position += to_copy;
        if (handle->position > handle->size) handle->size = handle->position;
    }
    
    if (bytes_written > 0) realfs_update_dir_entry_size(vol, handle);
    if (free_buf) kfree(cluster_buf);
    return bytes_written;
}


static bool realfs_delete_from_vol(FAT32_Volume *vol, const char *path) {
    if (!vol || !vol->mounted) return false;
    
    // Parse path to find start cluster and directory entry location
    uint32_t current_cluster = vol->root_cluster;
    
    const char *p = path;
    if (*p == '/') p++;
    
    if (*p == 0) {
        return false; // Cannot delete root
    }
    
    char component[256];
    uint32_t file_start_cluster = 0;
    uint32_t entry_sector = 0;
    uint32_t entry_offset = 0;
    bool is_directory = false;
    
    uint8_t *cluster_buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
    if (!cluster_buf) return false;
    
    while (*p) {
        // Extract component
        int i = 0;
        while (*p && *p != '/') {
            component[i++] = *p++;
        }
        component[i] = 0;
        if (*p == '/') p++; // Skip separator
        
        // Search in current_cluster
        bool found = false;
        uint32_t search_cluster = current_cluster;
        
        char lfn_buffer[256];
        bool has_lfn = false;
        for(int i=0; i<256; i++) lfn_buffer[i] = 0;

        while (search_cluster < 0x0FFFFFF8) {
            if (realfs_read_cluster(vol, search_cluster, cluster_buf) != 0) break;
            
            FAT32_DirEntry *entry = (FAT32_DirEntry*)cluster_buf;
            int entries_per_cluster = (vol->sectors_per_cluster * 512) / 32;
            
            for (int e = 0; e < entries_per_cluster; e++) {
                if (entry[e].filename[0] == 0) break;
                
                if (entry[e].filename[0] == 0xE5) {
                    has_lfn = false;
                    continue;
                }
                
                if (entry[e].attributes == ATTR_LFN) {
                    FAT32_LFNEntry *lfn = (FAT32_LFNEntry*)&entry[e];
                    if (lfn->order & 0x40) {
                        for(int i=0; i<256; i++) lfn_buffer[i] = 0;
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
                if (has_lfn && lfn_buffer[0] != 0) {
                    fs_strcpy(name, lfn_buffer);
                    has_lfn = false;
                } else {
                    int n = 0;
                    for (int k = 0; k < 8 && entry[e].filename[k] != ' '; k++) name[n++] = entry[e].filename[k];
                    if (entry[e].extension[0] != ' ') {
                        name[n++] = '.';
                        for (int k = 0; k < 3 && entry[e].extension[k] != ' '; k++) name[n++] = entry[e].extension[k];
                    }
                    name[n] = 0;
                }
                
                // Case insensitive compare
                bool match = true;
                int clen = fs_strlen(component);
                int nlen = fs_strlen(name);
                if (clen != nlen) match = false;
                else {
                    for (int c = 0; c < clen; c++) {
                        char c1 = name[c];
                        char c2 = component[c];
                        if (c1 >= 'a' && c1 <= 'z') c1 -= 32;
                        if (c2 >= 'a' && c2 <= 'z') c2 -= 32;
                        if (c1 != c2) { match = false; break; }
                    }
                }

                int lfn_start_entry = -1;
                if (has_lfn) {
                    // Find all LFN entries in reverse from the main entry
                    // LFN entries are marked by their order field and must be contiguous
                    for (int k = e - 1; k >= 0; k--) {
                        if (entry[k].attributes == ATTR_LFN) {
                            lfn_start_entry = k;  // Keep updating to find earliest LFN
                        } else {
                            // Stop when we hit a non-LFN entry that's not deleted
                            if (entry[k].filename[0] != 0xE5) break;
                        }
                    }
                }

                if (match) {
                    file_start_cluster = (entry[e].start_cluster_high << 16) | entry[e].start_cluster_low;
                    is_directory = (entry[e].attributes & ATTR_DIRECTORY) != 0;
                    
                    uint32_t lba = vol->cluster_begin_lba + (search_cluster - 2) * vol->sectors_per_cluster;
                    int sect_in_cluster = (e * 32) / 512;
                    entry_sector = lba + sect_in_cluster;
                    entry_offset = (e * 32) % 512;
                    
                    if (*p == 0) {
                        // Found target file/directory to delete
                        // Mark LFN entries as deleted too
                        if (lfn_start_entry != -1) {
                            for (int k = lfn_start_entry; k < e; k++) {
                                entry[k].filename[0] = 0xE5;
                            }
                        }
                        // Mark the main entry as deleted
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
                        
                        // Mark sector containing main entry
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
                        
                        // Write all modified sectors
                        for (int i = 0; i < num_sectors; i++) {
                            int sect_idx = sectors_to_write[i];
                            vol->disk->write_sector(vol->disk, lba + sect_idx, ((uint8_t*)entry) + (sect_idx * 512));
                        }
                        
                        found = true;
                    } else {
                        // It must be a directory to continue traversing
                        if (is_directory) {
                            current_cluster = file_start_cluster;
                            found = true;
                        }
                    }
                    break;
                }
            }
            if (found) break;
            search_cluster = realfs_next_cluster(vol, search_cluster);
        }
        
        if (!found) {
            kfree(cluster_buf);
            return false; // Path not found
        }
        
        if (*p == 0) break; // End of path
    }
    realfs_free_cluster_chain(vol, file_start_cluster);

    kfree(cluster_buf);
    return true;
}

static void extract_lfn_chars(FAT32_LFNEntry *lfn, char *buffer) {
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

static int realfs_list_directory_vol(FAT32_Volume *vol, const char *path, FAT32_FileInfo *entries, int max_entries, int offset) {
    if (!vol || !vol->mounted) return 0;
    FAT32_FileHandle *dir_handle = realfs_open_from_vol(vol, path, "r");
    if (!dir_handle) return 0;
    uint32_t current_cluster = dir_handle->start_cluster;
    extern void fat32_close_nolock(FAT32_FileHandle *handle);
    fat32_close_nolock(dir_handle);
    
    int count = 0;
    int found_so_far = 0;
    uint8_t *cluster_buf = (uint8_t*)kmalloc(vol->sectors_per_cluster * 512);
    char *lfn_buffer = (char*)kmalloc(256);
    char *name = (char*)kmalloc(256);
    if (!cluster_buf || !lfn_buffer || !name) {
       if (cluster_buf) kfree(cluster_buf);
       if (lfn_buffer) kfree(lfn_buffer);
       if (name) kfree(name);
       return 0;
    }
    bool has_lfn = false;
    for(int i=0; i<256; i++) lfn_buffer[i] = 0;

    while (current_cluster < 0x0FFFFFF8 && count < max_entries) {
        if (realfs_read_cluster(vol, current_cluster, cluster_buf) != 0) break;
        FAT32_DirEntry *de = (FAT32_DirEntry*)cluster_buf;
        for (int e = 0; e < (int)((vol->sectors_per_cluster*512)/32) && count < max_entries; e++) {
            if (de[e].filename[0] == 0) { current_cluster = 0xFFFFFFFF; break; }
            if (de[e].filename[0] == 0xE5) { has_lfn = false; continue; }
            if (de[e].filename[0] == 0x2E) { has_lfn = false; continue; } // Skip . and ..
            if (de[e].attributes == ATTR_LFN) {
                FAT32_LFNEntry *l = (FAT32_LFNEntry*)&de[e];
                if (l->order & 0x40) for(int k=0; k<256; k++) lfn_buffer[k] = 0;
                extract_lfn_chars(l, lfn_buffer);
                has_lfn = true; continue;
            }
            if (de[e].attributes & ATTR_VOLUME_ID) { has_lfn = false; continue; }
            
            if (found_so_far >= offset) {
                if (has_lfn && lfn_buffer[0] != 0) fs_strcpy(entries[count].name, lfn_buffer);
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
        if (current_cluster < 0x0FFFFFF8) current_cluster = realfs_next_cluster(vol, current_cluster);
    }
    kfree(cluster_buf); kfree(lfn_buffer); kfree(name);
    return count;
}



// ============================================================================
// VFS Adapters
// ============================================================================

static void vfs_ramfs_get_abs_path(const char *rel, char *abs) {
    if (rel[0] == '/') {
        fs_strcpy(abs, rel);
    } else {
        abs[0] = '/';
        fs_strcpy(abs + 1, rel);
    }
}

static void* vfs_ramfs_open(void *fs_private, const char *rel_path, const char *mode) {
    (void)fs_private;
    char *abs_path = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs_path) return NULL;
    vfs_ramfs_get_abs_path(rel_path, abs_path);

    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    void* handle = ramfs_open(abs_path, mode);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    kfree(abs_path);
    return handle;
}

static void vfs_ramfs_close(void *fs_private, void *file_handle) {
    (void)fs_private;
    fat32_close((FAT32_FileHandle*)file_handle);
}

static int vfs_ramfs_read(void *fs_private, void *file_handle, void *buf, int size) {
    (void)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    int ret = ramfs_read((FAT32_FileHandle*)file_handle, buf, size);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return ret;
}

static int vfs_ramfs_write(void *fs_private, void *file_handle, const void *buf, int size) {
    (void)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    int ret = ramfs_write((FAT32_FileHandle*)file_handle, buf, size);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return ret;
}

static int vfs_ramfs_seek(void *fs_private, void *file_handle, int offset, int whence) {
    (void)fs_private;
    return fat32_seek((FAT32_FileHandle*)file_handle, offset, whence);
}

static int vfs_ramfs_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset) {
    (void)fs_private;
    int count = 0;
    int found_so_far = 0;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return 0;
    vfs_ramfs_get_abs_path(rel_path, abs);
    
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    for (FileEntry *n = file_list_head; n && count < max; n = n->next) {
        bool match = false;
        if (n->filename[0] != '\0') {
            if (fs_strcmp(n->parent_path, abs) == 0) match = true;
            
            if (!match && abs[0] == '/' && abs[1] == '\0') {
                if (n->parent_path[0] == '\0' || 
                    fs_strcmp(n->parent_path, "/") == 0 ||
                    fs_strcmp(n->parent_path, "A:/") == 0) {
                    match = true;
                }
            }
        }
        
        if (match) {
            if (found_so_far >= offset) {
                fs_strcpy(entries[count].name, n->filename);
                entries[count].size = n->size;
                entries[count].is_directory = (n->attributes & ATTR_DIRECTORY) ? 1 : 0;
                entries[count].start_cluster = n->start_cluster;
                entries[count].write_date = 0;
                entries[count].write_time = 0;
                count++;
            }
            found_so_far++;
        }
    }
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    kfree(abs);
    return count;
}

static bool vfs_ramfs_mkdir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return false;
    vfs_ramfs_get_abs_path(rel_path, abs);
    bool ret = fat32_mkdir(abs);
    kfree(abs);
    return ret;
}

static bool vfs_ramfs_rmdir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return false;
    vfs_ramfs_get_abs_path(rel_path, abs);
    bool ret = fat32_rmdir(abs);
    kfree(abs);
    return ret;
}

static bool vfs_ramfs_unlink(void *fs_private, const char *rel_path) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return false;
    vfs_ramfs_get_abs_path(rel_path, abs);
    bool ret = fat32_delete(abs);
    kfree(abs);
    return ret;
}

static bool vfs_ramfs_rename(void *fs_private, const char *old_path, const char *new_path) {
    (void)fs_private;
    char *abs_old = (char*)kmalloc(FAT32_MAX_PATH);
    char *abs_new = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs_old || !abs_new) {
        if (abs_old) kfree(abs_old);
        if (abs_new) kfree(abs_new);
        return false;
    }
    vfs_ramfs_get_abs_path(old_path, abs_old);
    vfs_ramfs_get_abs_path(new_path, abs_new);
    bool ret = fat32_rename(abs_old, abs_new);
    kfree(abs_old);
    kfree(abs_new);
    return ret;
}

static bool vfs_ramfs_exists(void *fs_private, const char *rel_path) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return false;
    vfs_ramfs_get_abs_path(rel_path, abs);
    
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    bool exists = (ramfs_find_file(abs) != NULL);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    kfree(abs);
    return exists;
}

static bool vfs_ramfs_is_dir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return false;
    vfs_ramfs_get_abs_path(rel_path, abs);
    
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    FileEntry *entry = ramfs_find_file(abs);
    bool is_dir = (entry && (entry->attributes & ATTR_DIRECTORY));
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    kfree(abs);
    return is_dir;
}

static int vfs_ramfs_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info) {
    (void)fs_private;
    char *abs = (char*)kmalloc(FAT32_MAX_PATH);
    if (!abs) return -1;
    vfs_ramfs_get_abs_path(rel_path, abs);
    
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    FileEntry *entry = ramfs_find_file(abs);
    if (!entry) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        kfree(abs);
        return -1;
    }
    
    fs_strcpy(info->name, entry->filename);
    info->size = entry->size;
    info->is_directory = (entry->attributes & ATTR_DIRECTORY) ? 1 : 0;
    info->start_cluster = entry->start_cluster;
    info->write_date = 0;
    info->write_time = 0;
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    kfree(abs);
    return 0;
}

static uint32_t vfs_fat_get_position(void *file_handle) {
    return ((FAT32_FileHandle*)file_handle)->position;
}

static uint32_t vfs_fat_get_size(void *file_handle) {
    return ((FAT32_FileHandle*)file_handle)->size;
}

static int vfs_ramfs_statfs(void *fs_private, vfs_statfs_t *stat) {
    (void)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    stat->total_blocks = MAX_CLUSTERS;
    uint64_t free_count = 0;
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        if (fat_table[i] == 0) free_count++;
    }
    stat->free_blocks = free_count;
    stat->block_size = FAT32_CLUSTER_SIZE;
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return 0;
}

static struct vfs_fs_ops ramfs_ops = {
    .open = vfs_ramfs_open,
    .close = vfs_ramfs_close,
    .read = vfs_ramfs_read,
    .write = vfs_ramfs_write,
    .seek = vfs_ramfs_seek,
    .readdir = vfs_ramfs_readdir,
    .mkdir = vfs_ramfs_mkdir,
    .rmdir = vfs_ramfs_rmdir,
    .unlink = vfs_ramfs_unlink,
    .rename = vfs_ramfs_rename,
    .exists = vfs_ramfs_exists,
    .is_dir = vfs_ramfs_is_dir,
    .get_info = vfs_ramfs_get_info,
    .get_position = vfs_fat_get_position,
    .get_size = vfs_fat_get_size,
    .statfs = vfs_ramfs_statfs
};

struct vfs_fs_ops* fat32_get_ramfs_ops(void) {
    return &ramfs_ops;
}

// --- RealFS VFS Wrappers ---

static void* vfs_realfs_open(void *fs_private, const char *rel_path, const char *mode) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileHandle* fh = realfs_open_from_vol((FAT32_Volume*)fs_private, rel_path, mode);
    spinlock_release_irqrestore(&vol->lock, rflags);
    return fh;
}

static void vfs_realfs_close(void *fs_private, void *file_handle) {
    (void)fs_private;
    fat32_close((FAT32_FileHandle*)file_handle);
}

static int vfs_realfs_read(void *fs_private, void *file_handle, void *buf, int size) {
    (void)fs_private;
    FAT32_FileHandle *handle = (FAT32_FileHandle*)file_handle;
    FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
    
    // Allocate cluster buffer OUTSIDE the spinlock
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t*)kmalloc(cluster_size);
    if (!cluster_buf) return -1;

    int total_read = 0;
    while (total_read < size) {
        int to_read = size - total_read;
        if (to_read > (int)cluster_size) to_read = (int)cluster_size;

        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        int ret = realfs_read_file(handle, (uint8_t*)buf + total_read, to_read, cluster_buf);
        spinlock_release_irqrestore(&vol->lock, rflags);

        if (ret <= 0) break;
        total_read += ret;
        if (ret < to_read) break; 
    }

    kfree(cluster_buf);
    return total_read;
}

static int vfs_realfs_write(void *fs_private, void *file_handle, const void *buf, int size) {
    (void)fs_private;
    FAT32_FileHandle *handle = (FAT32_FileHandle*)file_handle;
    FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
    
    // Allocate cluster buffer OUTSIDE the spinlock
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t*)kmalloc(cluster_size);
    if (!cluster_buf) return -1;
    
    int total_written = 0;
    while (total_written < size) {
        int to_write = size - total_written;
        if (to_write > (int)cluster_size) to_write = (int)cluster_size;

        uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
        int ret = realfs_write_file(handle, (const uint8_t*)buf + total_written, to_write, cluster_buf);
        spinlock_release_irqrestore(&vol->lock, rflags);

        if (ret <= 0) break;
        total_written += ret;
        if (ret < to_write) break; 
    }

    if (total_written > 0) {
        fat32_sync_if_root(vol);
    }
    kfree(cluster_buf);
    return total_written;
}

static int vfs_realfs_seek(void *fs_private, void *file_handle, int offset, int whence) {
    (void)fs_private;
    return fat32_seek((FAT32_FileHandle*)file_handle, offset, whence);
}

static int vfs_realfs_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileInfo *fat_entries = (FAT32_FileInfo*)kmalloc(max * sizeof(FAT32_FileInfo));
    if (!fat_entries) { spinlock_release_irqrestore(&vol->lock, rflags); return 0; }
    
    int count = realfs_list_directory_vol((FAT32_Volume*)fs_private, rel_path, fat_entries, max, offset);
    for (int i = 0; i < count; i++) {
        fs_strcpy(entries[i].name, fat_entries[i].name);
        entries[i].size = fat_entries[i].size;
        entries[i].is_directory = fat_entries[i].is_directory;
        entries[i].write_date = fat_entries[i].write_date;
        entries[i].write_time = fat_entries[i].write_time;
    }
    
    kfree(fat_entries);
    spinlock_release_irqrestore(&vol->lock, rflags);
    return count;
}

static bool vfs_realfs_mkdir(void *fs_private, const char *rel_path) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    bool ret = realfs_mkdir_vol((FAT32_Volume*)fs_private, rel_path);
    spinlock_release_irqrestore(&vol->lock, rflags);
    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_realfs_rmdir(void *fs_private, const char *rel_path) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    FAT32_FileInfo child;
    bool ret = false;

    if (!vol || !rel_path || rel_path[0] == '\0' || fs_strcmp(rel_path, "/") == 0) {
        return false;
    }

    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileHandle *fh = realfs_open_from_vol(vol, rel_path, "r");
    if (!fh) {
        spinlock_release_irqrestore(&vol->lock, rflags);
        return false;
    }

    bool is_dir = fh->is_directory;
    extern void fat32_close_nolock(FAT32_FileHandle *handle);
    fat32_close_nolock(fh);

    if (is_dir && realfs_list_directory_vol(vol, rel_path, &child, 1, 0) == 0) {
        ret = realfs_delete_from_vol(vol, rel_path);
    }
    spinlock_release_irqrestore(&vol->lock, rflags);

    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_realfs_unlink(void *fs_private, const char *rel_path) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);

    FAT32_FileHandle *fh = realfs_open_from_vol(vol, rel_path, "r");
    if (!fh) {
        spinlock_release_irqrestore(&vol->lock, rflags);
        return false;
    }

    bool is_dir = fh->is_directory;
    extern void fat32_close_nolock(FAT32_FileHandle *handle);
    fat32_close_nolock(fh);

    bool ret = is_dir ? false : realfs_delete_from_vol(vol, rel_path);
    spinlock_release_irqrestore(&vol->lock, rflags);
    if (ret) fat32_sync_if_root(vol);
    return ret;
}

static bool vfs_realfs_rename(void *fs_private, const char *old_path, const char *new_path) {
    (void)fs_private; (void)old_path; (void)new_path;
    return false; // Not implemented yet for FAT32
}

static bool vfs_realfs_exists(void *fs_private, const char *rel_path) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileHandle *fh = realfs_open_from_vol((FAT32_Volume*)fs_private, rel_path, "r");
    if (fh) {
        extern void fat32_close_nolock(FAT32_FileHandle *handle);
        fat32_close_nolock(fh);
        spinlock_release_irqrestore(&vol->lock, rflags);
        return true;
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return false;
}

static bool vfs_realfs_is_dir(void *fs_private, const char *rel_path) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    if (fs_strcmp(rel_path, "/") == 0 || fs_strcmp(rel_path, "") == 0) return true;
    // Real implementation requires verifying DIR attribute
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileHandle *fh = realfs_open_from_vol((FAT32_Volume*)fs_private, rel_path, "r");
    bool is_dir = false;
    if (fh) {
        is_dir = fh->is_directory;
        // Limited metadata logic for now, best effort
        extern void fat32_close_nolock(FAT32_FileHandle *handle);
        fat32_close_nolock(fh);
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return is_dir; 
}

static int vfs_realfs_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    FAT32_FileHandle *fh = realfs_open_from_vol((FAT32_Volume*)fs_private, rel_path, "r");
    if (fh) {
        extract_filename(rel_path, info->name);
        info->size = fh->size;
        info->is_directory = fh->is_directory ? 1 : 0;
        extern void fat32_close_nolock(FAT32_FileHandle *handle);
        fat32_close_nolock(fh);
        spinlock_release_irqrestore(&vol->lock, rflags);
        return 0;
    }
    spinlock_release_irqrestore(&vol->lock, rflags);
    return -1;
}

static int vfs_realfs_statfs(void *fs_private, vfs_statfs_t *stat) {
    FAT32_Volume *vol = (FAT32_Volume*)fs_private;
    uint64_t rflags = spinlock_acquire_irqsave(&vol->lock);
    
    stat->total_blocks = vol->total_sectors / vol->sectors_per_cluster;
    stat->block_size = vol->sectors_per_cluster * 512;
    
    // Instead of scanning the entire FAT which can be slow, 
    // we estimate or count a subset, but let's do a fast count.
    uint64_t free_count = 0;
    uint32_t fat_entries = (vol->fat_size * 512) / 4;
    uint32_t current = 2;
    
    uint8_t *fat_buf = (uint8_t*)kmalloc(512);
    if (fat_buf) {
        uint32_t cached_sector = 0xFFFFFFFF;
        while (current < fat_entries) {
            uint32_t sector = vol->fat_begin_lba + (current * 4) / 512;
            uint32_t offset = (current * 4) % 512;
            
            if (sector != cached_sector) {
                if (vol->disk->read_sector(vol->disk, sector, fat_buf) != 0) break;
                cached_sector = sector;
            }
            
            uint32_t val = *(uint32_t*)&fat_buf[offset];
            if ((val & 0x0FFFFFFF) == 0) free_count++;
            
            current++;
        }
        kfree(fat_buf);
    }
    
    stat->free_blocks = free_count;
    
    spinlock_release_irqrestore(&vol->lock, rflags);
    return 0;
}

static struct vfs_fs_ops realfs_ops = {
    .open = vfs_realfs_open,
    .close = vfs_realfs_close,
    .read = vfs_realfs_read,
    .write = vfs_realfs_write,
    .seek = vfs_realfs_seek,
    .readdir = vfs_realfs_readdir,
    .mkdir = vfs_realfs_mkdir,
    .rmdir = vfs_realfs_rmdir,
    .unlink = vfs_realfs_unlink,
    .rename = vfs_realfs_rename,
    .exists = vfs_realfs_exists,
    .is_dir = vfs_realfs_is_dir,
    .get_info = vfs_realfs_get_info,
    .get_position = vfs_fat_get_position,
    .get_size = vfs_fat_get_size,
    .statfs = vfs_realfs_statfs
};

struct vfs_fs_ops* fat32_get_realfs_ops(void) {
    return &realfs_ops;
}

void* fat32_mount_volume(void *disk_ptr) {
    if (real_volume_count >= MAX_REAL_VOLUMES) return NULL;
    
    FAT32_Volume *vol = (FAT32_Volume*)kmalloc(sizeof(FAT32_Volume));
    if (!vol) return NULL;
    
    vol->mounted = false;
    vol->lock = SPINLOCK_INIT;
    if (realfs_mount_volume(vol, (Disk*)disk_ptr)) {
        real_volumes[real_volume_count++] = vol;
        return vol;
    }
    
    kfree(vol);
    return NULL;
}


// === Public API (Dispatch) ===

void fat32_init(void) {
    FileEntry *node = file_list_head;
    while (node) {
        FileEntry *next = node->next;
        kfree(node);
        node = next;
    }
    file_list_head = NULL;

    // Initialize FAT table for RAMFS
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        fat_table[i] = 0;
    }
    fat_table[0] = 0xFFFFFFF8;
    fat_table[1] = 0xFFFFFFFF;

    // Zero out cluster data
    for (int i = 0; i < MAX_CLUSTERS; i++) {
        for (int j = 0; j < FAT32_CLUSTER_SIZE; j++) {
            cluster_data[i][j] = 0;
        }
    }
    
    // Reserve cluster 2 for root directory
    fat_table[2] = 0xFFFFFFFF;
    next_cluster = 3;

    // Register with VFS as root mount
    vfs_mount("/", "ramfs", "ramfs", fat32_get_ramfs_ops(), NULL);

    current_dir[0] = '/';
    current_dir[1] = 0;
    current_drive = 'A';
    
    // Reset Volumes
    for(int i=0; i<MAX_REAL_VOLUMES; i++) real_volumes[i] = NULL;
    real_volume_count = 0;
}

void fat32_set_desktop_limit(int limit) {
    desktop_file_limit = limit;
}

bool fat32_change_drive(char drive) {
    (void)drive;
    return false; // Obsolete in VFS
}

char fat32_get_current_drive(void) {
    return current_drive;
}

FAT32_FileHandle* fat32_open_nolock(const char *path, const char *mode) {
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    FAT32_FileHandle *handle = NULL;
    if (drive == 'A') {
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (!normalized) return NULL;
        fat32_normalize_path(p, normalized);
        handle = ramfs_open(normalized, mode);
        kfree(normalized);
    } else if (drive == 0) {
        handle = realfs_open_from_vol(root_volume, p, mode);
    } else if (drive != 0) {
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    handle = realfs_open_from_vol(real_volumes[i], p, mode);
                    break;
                }
            }
        }
    } else if (path[0] == '/') {
        vfs_file_t *vf = vfs_open(path, mode);
        if (vf && vf->fs_handle) {
             return (FAT32_FileHandle*)vf->fs_handle;
        }
    }
    return handle;
}

FAT32_FileHandle* fat32_open(const char *path, const char *mode) {
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    FAT32_FileHandle* handle = fat32_open_nolock(path, mode);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return handle;
}

void fat32_close_nolock(FAT32_FileHandle *handle) {
    if (handle && handle->valid) {
        if (handle->volume != NULL && handle->mode != 0) {  // Both read and write modes for real drives
            FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
            Disk *d = vol->disk;
            if (d && handle->dir_sector != 0) {
                 uint8_t *buf = (uint8_t*)kmalloc(512);
                 if (buf) {
                     if (d->read_sector(d, handle->dir_sector, buf) == 0) {
                         FAT32_DirEntry *entry = (FAT32_DirEntry*)(buf + handle->dir_offset);
                         entry->file_size = handle->size;
                         if (handle->start_cluster != 0) {
                             entry->start_cluster_high = (handle->start_cluster >> 16);
                             entry->start_cluster_low = (handle->start_cluster & 0xFFFF);
                         }
                         d->write_sector(d, handle->dir_sector, buf);
                     }
                     kfree(buf);
                 }
            }
            fat32_sync_if_root(vol);
        }
        handle->valid = false;
    }
}

void fat32_close(FAT32_FileHandle *handle) {
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    fat32_close_nolock(handle);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
}

int fat32_read(FAT32_FileHandle *handle, void *buffer, int size) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    if (!handle || !handle->valid || handle->mode != 0) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return -1;
    }
    
    int ret = 0;
    if (handle->volume == NULL) {
        ret = ramfs_read(handle, buffer, size);
    } else {
        ret = realfs_read_file(handle, buffer, size, NULL);
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return ret;
}

int fat32_write(FAT32_FileHandle *handle, const void *buffer, int size) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    if (!handle || !handle->valid || (handle->mode != 1 && handle->mode != 2)) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return -1;
    }
    
    int ret = 0;
    if (handle->volume == NULL) {
        ret = ramfs_write(handle, buffer, size);
    } else {
        ret = realfs_write_file(handle, buffer, size, NULL);
        if (ret > 0) {
            FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
            fat32_sync_if_root(vol);
        }
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return ret;
}

int fat32_seek(FAT32_FileHandle *handle, int offset, int whence) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    if (!handle || !handle->valid) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return -1;
    }
    
    uint32_t new_position = handle->position;
    if (whence == 0) new_position = offset;
    else if (whence == 1) new_position += offset;
    else if (whence == 2) new_position = handle->size + offset;
    
    if (new_position > handle->size) new_position = handle->size;
    
    handle->position = new_position;
    
    // Both RealFS and RAMFS must accurately re-walk their cluster chains
    if (handle->volume == NULL) {
        handle->cluster = handle->start_cluster;
        uint32_t pos = 0;
        while (pos + FAT32_CLUSTER_SIZE <= handle->position) {
             uint32_t next = fat_table[handle->cluster];
             if (next >= 0xFFFFFFF8) break;
             handle->cluster = next;
             pos += FAT32_CLUSTER_SIZE;
        }
    } else {
        // Re-walk to find current cluster
        FAT32_Volume *vol = (FAT32_Volume*)handle->volume;
        uint32_t cluster_size = vol->sectors_per_cluster * 512;
        
        handle->cluster = handle->start_cluster;
        uint32_t pos = 0;
        while (pos + cluster_size <= handle->position) {
             uint32_t next = realfs_next_cluster(vol, handle->cluster);
             if (next >= 0x0FFFFFF8) break;
             handle->cluster = next;
             pos += cluster_size;
        }
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return new_position;
}

static bool realfs_mkdir_vol(FAT32_Volume *vol, const char *path) {
    if (!vol || !vol->mounted) return false;

    // Find parent directory and name of new directory
    char *parent_path = (char*)kmalloc(FAT32_MAX_PATH);
    if (!parent_path) return false;
    char dirname[FAT32_MAX_FILENAME];
    extract_parent_path(path, parent_path);
    extract_filename(path, dirname);

    // Open parent directory
    FAT32_FileHandle *parent_fh = realfs_open_from_vol(vol, parent_path, "r");
    kfree(parent_path);
    if (!parent_fh) {
        serial_write("[FAT32] mkdir ERROR: parent not found\n");
        return false;
    }
    uint32_t parent_cluster = parent_fh->start_cluster;

    extern void fat32_close_nolock(FAT32_FileHandle *handle);
    fat32_close_nolock(parent_fh);

    FAT32_FileHandle *check_fh = realfs_open_from_vol(vol, path, "r");
    if (check_fh) {
        extern void fat32_close_nolock(FAT32_FileHandle *handle);
        fat32_close_nolock(check_fh);
        return false;
    }

    // Allocate cluster for new directory
    uint32_t new_cluster = realfs_allocate_cluster(vol);
    if (new_cluster == 0) return false;

    // Initialize new directory cluster with . and .. entries
    uint32_t cluster_size = vol->sectors_per_cluster * 512;
    uint8_t *cluster_buf = (uint8_t*)kmalloc(cluster_size);
    if (!cluster_buf) return false;
    for (uint32_t i = 0; i < cluster_size; i++) cluster_buf[i] = 0;

    FAT32_DirEntry *dot = (FAT32_DirEntry*)cluster_buf;
    FAT32_DirEntry *dotdot = (FAT32_DirEntry*)(cluster_buf + 32);

    // . entry
    for (int i = 0; i < 8; i++) dot->filename[i] = ' ';
    for (int i = 0; i < 3; i++) dot->extension[i] = ' ';
    dot->filename[0] = '.';
    dot->attributes = ATTR_DIRECTORY;
    dot->start_cluster_high = (new_cluster >> 16);
    dot->start_cluster_low = (new_cluster & 0xFFFF);

    // .. entry 
    for (int i = 0; i < 8; i++) dotdot->filename[i] = ' ';
    for (int i = 0; i < 3; i++) dotdot->extension[i] = ' ';
    dotdot->filename[0] = '.'; dotdot->filename[1] = '.';
    dotdot->attributes = ATTR_DIRECTORY;
    dotdot->start_cluster_high = (parent_cluster >> 16);
    dotdot->start_cluster_low = (parent_cluster & 0xFFFF);

    if (realfs_write_cluster(vol, new_cluster, cluster_buf) != 0) {
        kfree(cluster_buf);
        return false;
    }
    kfree(cluster_buf);

    uint32_t free_sector = 0;
    uint32_t free_offset = 0;

    if (!realfs_create_entry(vol, parent_cluster, dirname, ATTR_DIRECTORY, new_cluster, 0, &free_sector, &free_offset)) {
        return false;
    }

    return true;
}

bool fat32_mkdir(const char *path) {
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);

    if (drive == 0) {
        bool res = realfs_mkdir_vol(root_volume, p);
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return res;
    } else if (drive != 'A') {
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    bool res = realfs_mkdir_vol(real_volumes[i], p);
                    spinlock_release_irqrestore(&ramfs_lock, rflags);
                    return res;
                }
            }
        }
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }

    char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
    if (!normalized) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }
    fat32_normalize_path(p, normalized);
    
    if (ramfs_find_file(normalized)) {
        kfree(normalized);
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false; 
    }
    
    if (!check_desktop_limit(normalized)) {
        kfree(normalized);
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }
    
    FileEntry *entry = ramfs_alloc_entry();
    if (!entry) {
        kfree(normalized);
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }
    
    entry->used = true;
    fs_strcpy(entry->full_path, normalized);
    extract_filename(normalized, entry->filename);
    extract_parent_path(normalized, entry->parent_path);
    entry->start_cluster = ramfs_allocate_cluster();
    entry->size = 0;
    entry->attributes = ATTR_DIRECTORY;
    
    kfree(normalized);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return true;
}

bool fat32_rmdir(const char *path) {
    if (parse_drive_from_path(&path) != 'A') return false;
    
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
    if (!normalized) {
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }
    fat32_normalize_path(path, normalized);
    
    FileEntry *entry = ramfs_find_file(normalized);
    if (!entry || !(entry->attributes & ATTR_DIRECTORY)) {
        kfree(normalized);
        spinlock_release_irqrestore(&ramfs_lock, rflags);
        return false;
    }

    for (FileEntry *n = file_list_head; n; n = n->next) {
        if (n != entry && fs_strcmp(n->parent_path, normalized) == 0) {
            kfree(normalized);
            spinlock_release_irqrestore(&ramfs_lock, rflags);
            return false;
        }
    }
    
    ramfs_free_entry(entry);
    kfree(normalized);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return true;
}

bool fat32_delete(const char *path) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    bool result = false;
    
    if (drive == 'A') {
        // RAMFS deletion
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (normalized) {
            fat32_normalize_path(p, normalized);
            
            FileEntry *entry = ramfs_find_file(normalized);
            if (entry && !(entry->attributes & ATTR_DIRECTORY)) {
                ramfs_free_entry(entry);
                result = true;
            }
            kfree(normalized);
        }
    } else if (drive == 0) {
        result = realfs_delete_from_vol(root_volume, p);
    } else {
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    result = realfs_delete_from_vol(real_volumes[i], p);
                    break;
                }
            }
        }
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return result;
}

int fat32_get_info(const char *path, FAT32_FileInfo *info) {
    if (path[0] == '/') {
        // Absolute path - route via VFS
        vfs_dirent_t v_info;
        int res = vfs_get_info(path, &v_info);
        if (res == 0) {
            fs_strcpy(info->name, v_info.name);
            info->size = v_info.size;
            info->is_directory = v_info.is_directory;
            info->start_cluster = v_info.start_cluster;
            info->write_date = v_info.write_date;
            info->write_time = v_info.write_time;
            return 0;
        }
        return -1;
    }

    // Legacy drive-based path
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    int result = -1;
    if (drive == 'A') {
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (normalized) {
            fat32_normalize_path(p, normalized);
            FileEntry *entry = ramfs_find_file(normalized);
            if (entry) {
                fs_strcpy(info->name, entry->filename);
                info->size = entry->size;
                info->is_directory = (entry->attributes & ATTR_DIRECTORY) != 0;
                info->start_cluster = entry->start_cluster;
                result = 0;
            }
            kfree(normalized);
        }
    } else {
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    FAT32_FileHandle *fh = realfs_open_from_vol(real_volumes[i], p, "r");
                    if (fh) {
                        extract_filename(p, info->name);
                        info->size = fh->size;
                        info->start_cluster = fh->start_cluster;
                        
                        if (fs_strcmp(p, "/") == 0 || fs_strcmp(p, "") == 0) {
                            info->is_directory = 1;
                        } else {
                            // Temporary: Assume if it opens as "r" and it's not root, 
                            // we'd need better dir check. For now just 0.
                            info->is_directory = 0;
                        }
                        extern void fat32_close_nolock(FAT32_FileHandle *handle);
                        fat32_close_nolock(fh);
                        result = 0;
                        break;
                    }
                }
            }
        }
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return result;
}

bool fat32_exists(const char *path) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    bool exists = false;
    if (drive == 'A') {
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (normalized) {
            fat32_normalize_path(p, normalized);
            exists = (ramfs_find_file(normalized) != NULL);
            kfree(normalized);
        }
    } else if (drive == 0) {
        FAT32_FileHandle *fh = realfs_open_from_vol(root_volume, p, "r");
        if (fh) {
            exists = true;
            fat32_close_nolock(fh);
        }
    } else {
        // RealFS check
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    FAT32_FileHandle *fh = realfs_open_from_vol(real_volumes[i], p, "r");
                    if (fh) {
                        exists = true;
                        extern void fat32_close_nolock(FAT32_FileHandle *handle);
                        fat32_close_nolock(fh);
                        break;
                    }
                }
            }
        }
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return exists;
}

bool fat32_rename(const char *old_path, const char *new_path) {
    // Only A: supported for rename/modify
    if (parse_drive_from_path(&old_path) != 'A') return false;
    if (parse_drive_from_path(&new_path) != 'A') return false;

    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    FileEntry *entry = ramfs_find_file(old_path); // Need to normalize inside find? yes ramfs_find calls normalize
    if (!entry) { spinlock_release_irqrestore(&ramfs_lock, rflags); return false; }
    
    // Check destination
    if (ramfs_find_file(new_path)) { spinlock_release_irqrestore(&ramfs_lock, rflags); return false; }

    size_t old_len = fs_strlen(old_path);
    // Logic from original rename...
    char *suffix = (char*)kmalloc(FAT32_MAX_PATH);
    if (!suffix) { spinlock_release_irqrestore(&ramfs_lock, rflags); return false; }

    for (FileEntry *n = file_list_head; n; n = n->next) {
        if (fs_strcmp(n->full_path, old_path) == 0) {
            fs_strcpy(n->full_path, new_path);
            extract_filename(new_path, n->filename);
            extract_parent_path(new_path, n->parent_path);
        } else if (fs_strlen(n->full_path) > old_len &&
                   fs_starts_with(n->full_path, old_path) &&
                   n->full_path[old_len] == '/') {
            fs_strcpy(suffix, n->full_path + old_len);
            fs_strcpy(n->full_path, new_path);
            fs_strcat(n->full_path, suffix);
        }
        if (fs_strcmp(n->parent_path, old_path) == 0) {
            fs_strcpy(n->parent_path, new_path);
        } else if (fs_strlen(n->parent_path) > old_len &&
                   fs_starts_with(n->parent_path, old_path) &&
                   n->parent_path[old_len] == '/') {
            fs_strcpy(suffix, n->parent_path + old_len);
            fs_strcpy(n->parent_path, new_path);
            fs_strcat(n->parent_path, suffix);
        }
    }
    kfree(suffix);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return true;
}

bool fat32_is_directory_nolock(const char *path) {
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    bool is_dir = false;
    if (drive == 'A') {
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (normalized) {
            fat32_normalize_path(p, normalized);
            FileEntry *entry = ramfs_find_file(normalized);
            is_dir = (entry && (entry->attributes & ATTR_DIRECTORY));
            kfree(normalized);
        }
    } else if (drive == 0) {
        FAT32_FileHandle *fh = realfs_open_from_vol(root_volume, p, "r");
        if (fh) {
            is_dir = fh->is_directory;
            fat32_close_nolock(fh);
        }
    } else {
        Disk *d = disk_get_by_letter(drive);
        if (d) {
            for (int i = 0; i < real_volume_count; i++) {
                if (real_volumes[i]->disk == d) {
                    FAT32_FileHandle *fh = realfs_open_from_vol(real_volumes[i], p, "r");
                    if (fh) {
                        is_dir = fh->is_directory;
                        fat32_close_nolock(fh);
                        break;
                    }
                }
            }
        }
    }
    return is_dir;
}

bool fat32_is_directory(const char *path) {
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    bool is_dir = fat32_is_directory_nolock(path);
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return is_dir;
}

int fat32_list_directory(const char *path, FAT32_FileInfo *entries, int max_entries) {
    if (path[0] == '/') {
        // Absolute path - Route through unified VFS
        vfs_dirent_t *v_entries = (vfs_dirent_t*)kmalloc(sizeof(vfs_dirent_t) * max_entries);
        if (!v_entries) return 0;
        
        int count = vfs_list_directory(path, v_entries, max_entries, 0);
        for (int i = 0; i < count; i++) {
            fs_strcpy(entries[i].name, v_entries[i].name);
            entries[i].size = v_entries[i].size;
            entries[i].is_directory = v_entries[i].is_directory;
            entries[i].start_cluster = v_entries[i].start_cluster;
            entries[i].write_date = v_entries[i].write_date;
            entries[i].write_time = v_entries[i].write_time;
        }
        kfree(v_entries);
        return count;
    }

    // Legacy drive-based path (A:, B: etc)
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    int count = 0;
    if (drive == 'A') {
        char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
        if (!normalized) { spinlock_release_irqrestore(&ramfs_lock, rflags); return 0; }
        fat32_normalize_path(p, normalized);
        
        for (FileEntry *_n = file_list_head; _n && count < max_entries; _n = _n->next) {
                if (fs_strcmp(_n->parent_path, normalized) != 0) continue;
                fs_strcpy(entries[count].name, _n->filename);
                entries[count].size = _n->size;
                entries[count].is_directory = (_n->attributes & ATTR_DIRECTORY) != 0;
                entries[count].start_cluster = _n->start_cluster;
                entries[count].write_date = 0;
                entries[count].write_time = 0;
                count++;
            }
        kfree(normalized);
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return count;
}

bool fat32_chdir(const char *path) {
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    const char *p = path;
    char drive = parse_drive_from_path(&p);
    
    if (path[0] && path[1] == ':') {
         if (disk_get_by_letter(drive)) {
             current_drive = drive;
             current_dir[0] = '/';
             current_dir[1] = 0;
             if (p[0] == 0) {
                 spinlock_release_irqrestore(&ramfs_lock, rflags);
                 return true;
             }
         } else {
             spinlock_release_irqrestore(&ramfs_lock, rflags);
             return false;
         }
    }
    
    if (fat32_is_directory_nolock(path)) {
         if (drive == 'A') {
             char *normalized = (char*)kmalloc(FAT32_MAX_PATH);
             if (normalized) {
                 fat32_normalize_path(p, normalized);
                 fs_strcpy(current_dir, normalized);
                 kfree(normalized);
             }
         } else {
             fs_strcpy(current_dir, p); 
             if (current_dir[0] != '/') {

             }
         }
         spinlock_release_irqrestore(&ramfs_lock, rflags);
         return true;
    }
    
    spinlock_release_irqrestore(&ramfs_lock, rflags);
    return false;
}

void fat32_get_current_dir(char *buffer, int size) {
    // SMP: Use FAT32 spinlock
    uint64_t rflags = spinlock_acquire_irqsave(&ramfs_lock);
    
    int len = 0;
    buffer[0] = current_drive;
    buffer[1] = ':';
    len = 2;
    
    int dir_len = fs_strlen(current_dir);
    if (len + dir_len >= size) dir_len = size - len - 1;
    
    for (int i = 0; i < dir_len; i++) {
        buffer[len + i] = current_dir[i];
    }
    buffer[len + dir_len] = 0;
    spinlock_release_irqrestore(&ramfs_lock, rflags);
}
