// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "bootfs.h"
#include "disk.h"
#include "fat32.h"
#include "bootfs_state.h"
#include "vfs.h"
#include "kutils.h"
#include "platform.h"
#include "kconsole.h"
#include "memory_manager.h"

extern void serial_write(const char *str);
extern void serial_write_hex(uint64_t value);

typedef struct bootfs_custom_file {
    char name[128];       /* filename relative to /boot, e.g. "boredos.elf" */
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;    /* 0 = read-only initrd pointer; >0 = heap-allocated writable */
    struct bootfs_custom_file *next;
} bootfs_custom_file_t;

typedef struct {
    char path[512];
    int offset;
    bool is_root;
    bool is_metadata_dir;
    vfs_file_t *disk_file;
} bootfs_handle_t;

static void* bootfs_open(void *fs_private, const char *path, const char *mode);
static void bootfs_close(void *fs_private, void *handle);
static int bootfs_read(void *fs_private, void *handle, void *buf, size_t size);
static int bootfs_write(void *fs_private, void *handle, const void *buf, size_t size);
static int bootfs_seek(void *fs_private, void *handle, int offset, int whence);
static int bootfs_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset);
static bool bootfs_mkdir(void *fs_private, const char *rel_path);
static bool bootfs_rmdir(void *fs_private, const char *rel_path);
static bool bootfs_unlink(void *fs_private, const char *rel_path);
static bool bootfs_rename(void *fs_private, const char *old_path, const char *new_path);
static bool bootfs_exists(void *fs_private, const char *rel_path);
static bool bootfs_is_dir(void *fs_private, const char *rel_path);
static int bootfs_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info);
static uint32_t bootfs_get_position(void *file_handle);
static uint32_t bootfs_get_size(void *file_handle);

static vfs_fs_ops_t bootfs_ops = {
    .open = bootfs_open,
    .close = bootfs_close,
    .read = bootfs_read,
    .write = bootfs_write,
    .seek = bootfs_seek,
    .readdir = bootfs_readdir,
    .mkdir = bootfs_mkdir,
    .rmdir = bootfs_rmdir,
    .unlink = bootfs_unlink,
    .rename = bootfs_rename,
    .exists = bootfs_exists,
    .is_dir = bootfs_is_dir,
    .get_info = bootfs_get_info,
    .get_position = bootfs_get_position,
    .get_size = bootfs_get_size,
};

bootfs_state_t g_bootfs_state = {0};
static char g_limine_conf_path[64] = "";

static bootfs_custom_file_t *bootfs_find_custom(const char *name) {
    bootfs_custom_file_t *f = (bootfs_custom_file_t*)g_bootfs_state.custom_files;
    while (f) {
        if (strcmp(f->name, name) == 0) return f;
        f = f->next;
    }
    return NULL;
}

void bootfs_register_file(const char *name, void *data, uint32_t size) {
    if (!name || !data) return;
    bootfs_custom_file_t *f = bootfs_find_custom(name);
    if (!f) {
        f = (bootfs_custom_file_t*)kmalloc(sizeof(bootfs_custom_file_t));
        if (!f) return;
        memset(f, 0, sizeof(bootfs_custom_file_t));
        strcpy(f->name, name);
        f->next = (bootfs_custom_file_t*)g_bootfs_state.custom_files;
        g_bootfs_state.custom_files = f;
    }
    f->data = (uint8_t*)kmalloc(size);
    if (f->data) {
        memcpy(f->data, data, size);
    }
    f->size = size;
    f->capacity = size;
}

static bool is_metadata_path(const char *path) {
    if (!path) return false;
    return strncmp(path, "metadata", 8) == 0;
}

static bool is_metadata_file(const char *path) {
    if (strcmp(path, "metadata/boot_time") == 0) return true;
    if (strcmp(path, "metadata/boot_flags") == 0) return true;
    if (strcmp(path, "metadata/version") == 0) return true;
    return false;
}

static void* bootfs_open(void *fs_private, const char *path, const char *mode) {
    if (!path) path = "";
    if (path[0] == '/') path++;
    
    bootfs_handle_t *h = (bootfs_handle_t*)kmalloc(sizeof(bootfs_handle_t));
    if (!h) return NULL;
    
    memset(h, 0, sizeof(bootfs_handle_t));
    strcpy(h->path, path);
    h->offset = 0;
    h->disk_file = NULL;
    
    if (path[0] == '\0') {
        h->is_root = true;
    } else if (is_metadata_path(path) && path[8] == '\0') {
        h->is_metadata_dir = true;
    } else if (strcmp(path, "kernel") == 0 || strcmp(path, "initrd") == 0 || strcmp(path, "initrd.tar") == 0 || is_metadata_file(path) || bootfs_find_custom(path)) {
    } else {
        if (strncmp(path, "efi/", 4) == 0 || strcmp(path, "efi") == 0) {
            kfree(h);
            return NULL;
        }
        char disk_path[256];
        vfs_file_t *f = NULL;
        
        strcpy(disk_path, "/boot/efi/");
        strcat(disk_path, path);
        f = vfs_open(disk_path, mode);
        if (!f) {
            strcpy(disk_path, "/");
            strcat(disk_path, path);
            f = vfs_open(disk_path, mode);
        }
        
        if (f) {
            h->disk_file = f;
        } else {
            if (mode[0] == 'w' || mode[0] == 'a') {
                strcpy(disk_path, "/boot/efi/");
                strcat(disk_path, path);
                f = vfs_open(disk_path, mode);
                if (!f) {
                    strcpy(disk_path, "/");
                    strcat(disk_path, path);
                    f = vfs_open(disk_path, mode);
                }
                h->disk_file = f;
            }
        }
        
        if (!h->disk_file) {
            kfree(h);
            return NULL;
        }
    }
    
    return h;
}

static void bootfs_close(void *fs_private, void *handle) {
    bootfs_handle_t *h = (bootfs_handle_t*)handle;
    if (h) {
        if (h->disk_file) {
            vfs_close(h->disk_file);
        }
        kfree(h);
    }
}

static int generate_metadata_content(const char *file, char *buffer, int max_size) {
    if (!buffer || max_size <= 0) return 0;
    
    buffer[0] = '\0';
    int len = 0;
    
    if (strcmp(file, "metadata/boot_time") == 0) {
        extern uint32_t get_ticks(void);
        uint32_t ticks = get_ticks();
        
        strcpy(buffer, "Boot time: ");
        char time_buf[32];
        itoa(g_bootfs_state.boot_time_ms, time_buf);
        strcpy(buffer + strlen(buffer), time_buf);
        strcpy(buffer + strlen(buffer), " ms\nTicks: ");
        itoa(ticks, time_buf);
        strcpy(buffer + strlen(buffer), time_buf);
        strcpy(buffer + strlen(buffer), "\n");
        len = strlen(buffer);
    } else if (strcmp(file, "metadata/version") == 0) {
        strcpy(buffer, "Bootloader: ");
        strcpy(buffer + strlen(buffer), g_bootfs_state.bootloader_name);
        strcpy(buffer + strlen(buffer), "\nVersion: ");
        strcpy(buffer + strlen(buffer), g_bootfs_state.bootloader_version);
        strcpy(buffer + strlen(buffer), "\n");
        len = strlen(buffer);
    } else if (strcmp(file, "metadata/boot_flags") == 0) {
        strcpy(buffer, "Boot flags: 0x");
        char flags_buf[8];
        uint8_t flags = g_bootfs_state.boot_flags;
        int hex_digit = (flags >> 4) & 0xF;
        flags_buf[0] = hex_digit < 10 ? '0' + hex_digit : 'a' + (hex_digit - 10);
        hex_digit = flags & 0xF;
        flags_buf[1] = hex_digit < 10 ? '0' + hex_digit : 'a' + (hex_digit - 10);
        flags_buf[2] = '\n';
        flags_buf[3] = '\0';
        strcpy(buffer + strlen(buffer), flags_buf);
        len = strlen(buffer);
    }
    
    return len;
}

static int bootfs_read(void *fs_private, void *handle, void *buf, size_t size) {
    bootfs_handle_t *h = (bootfs_handle_t*)handle;
    if (!h || !buf || size <= 0) return -1;
    
    if (h->disk_file) {
        int ret = vfs_read(h->disk_file, buf, size);
        if (ret > 0) h->offset += ret;
        return ret;
    }
    
    char *content_buffer = (char*)kmalloc(4096);
    if (!content_buffer) return -1;
    
    int content_len = 0;
    
    if (strcmp(h->path, "kernel") == 0) {
        strcpy(content_buffer, "Kernel reference\nSize: ");
        char size_buf[32];
        itoa(g_bootfs_state.kernel_size, size_buf);
        strcpy(content_buffer + strlen(content_buffer), size_buf);
        strcpy(content_buffer + strlen(content_buffer), " bytes\n");
        content_len = strlen(content_buffer);
    } else if (strcmp(h->path, "initrd") == 0) {
        strcpy(content_buffer, "Initial ramdisk reference\nSize: ");
        char size_buf[32];
        itoa(g_bootfs_state.initrd_size, size_buf);
        strcpy(content_buffer + strlen(content_buffer), size_buf);
        strcpy(content_buffer + strlen(content_buffer), " bytes\n");
        content_len = strlen(content_buffer);
    } else if (strcmp(h->path, "initrd.tar") == 0) {
        kfree(content_buffer);
        if (h->offset >= (int)g_bootfs_state.initrd_size) return 0;
        int avail = (int)g_bootfs_state.initrd_size - h->offset;
        int to_read = (size < avail) ? size : avail;
        memcpy(buf, (uint8_t*)g_bootfs_state.initrd_ptr + h->offset, to_read);
        h->offset += to_read;
        return to_read;
    } else if (is_metadata_file(h->path)) {
        content_len = generate_metadata_content(h->path, content_buffer, 4096);
    } else {
        bootfs_custom_file_t *cf = bootfs_find_custom(h->path);
        if (cf) {
            kfree(content_buffer);
            if (h->offset >= (int)cf->size) return 0;
            int avail = (int)cf->size - h->offset;
            int to_read = (avail < size) ? avail : size;
            memcpy(buf, cf->data + h->offset, to_read);
            h->offset += to_read;
            return to_read;
        }
        kfree(content_buffer);
        return -1;
    }
    
    // Handle offset and reading
    if (h->offset >= content_len) {
        kfree(content_buffer);
        return 0;
    }
    
    int available = content_len - h->offset;
    int read_size = (available < size) ? available : size;
    
    memcpy(buf, content_buffer + h->offset, read_size);
    h->offset += read_size;
    
    kfree(content_buffer);
    return read_size;
}

static int bootfs_write(void *fs_private, void *handle, const void *buf, size_t size) {
    bootfs_handle_t *h = (bootfs_handle_t*)handle;
    if (!h || !buf || size <= 0) return -1;
    
    if (h->disk_file) {
        int ret = vfs_write(h->disk_file, buf, size);
        if (ret > 0) h->offset += ret;
        return ret;
    }
    
    return -1;
}

static int bootfs_seek(void *fs_private, void *handle, int offset, int whence) {
    bootfs_handle_t *h = (bootfs_handle_t*)handle;
    if (!h) return -1;
    
    if (h->disk_file) {
        int ret = vfs_seek(h->disk_file, offset, whence);
        if (ret >= 0) h->offset = ret;
        return ret;
    }
    
    switch (whence) {
        case 0: // SEEK_SET
            h->offset = offset;
            break;
        case 1: // SEEK_CUR
            h->offset += offset;
            break;
        case 2: // SEEK_END
            return -1;
        default:
            return -1;
    }
    
    return h->offset;
}

static int bootfs_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset) {
    if (!entries || max <= 0) return 0;
    
    if (!rel_path) rel_path = "";
    if (rel_path[0] == '/') rel_path++;
    
    if (strncmp(rel_path, "efi/", 4) == 0 || strcmp(rel_path, "efi") == 0) {
        return 0;
    }
    
    int count = 0;
    int found_so_far = 0;
    
    if (rel_path[0] == '\0') {
        const char *boot_files[] = {
            "kernel", "initrd", "initrd.tar", "metadata"
        };
        for (int i = 0; i < 4; i++) {
            if (found_so_far >= offset) {
                strcpy(entries[count].name, boot_files[i]);
                if (strcmp(boot_files[i], "kernel") == 0) {
                    entries[count].size = g_bootfs_state.kernel_size;
                    entries[count].is_directory = 0;
                } else if (strcmp(boot_files[i], "initrd") == 0 || strcmp(boot_files[i], "initrd.tar") == 0) {
                    entries[count].size = g_bootfs_state.initrd_size;
                    entries[count].is_directory = 0;
                } else if (strcmp(boot_files[i], "metadata") == 0) {
                    entries[count].size = 0;
                    entries[count].is_directory = 1;
                }
                count++;
                if (count >= max) return count;
            }
            found_so_far++;
        }

        // Dynamically add bootloader configuration files if they exist on the ESP or root partition
        vfs_dirent_t part_entries[64];
        int part_count = vfs_list_directory("/boot/efi", part_entries, 64, 0);
        if (part_count <= 0) {
            part_count = vfs_list_directory("/", part_entries, 64, 0);
        }
        
        for (int i = 0; i < part_count; i++) {
            if (part_count > 0 && strcmp(part_entries[i].name, "sys") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "proc") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "dev") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "boot") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "bin") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "initrd.tar.lz4") == 0) continue;
            if (part_count > 0 && strcmp(part_entries[i].name, "boredos.elf") == 0) continue;
            
            if (found_so_far >= offset) {
                strcpy(entries[count].name, part_entries[i].name);
                entries[count].size = part_entries[i].size;
                entries[count].is_directory = part_entries[i].is_directory;
                count++;
                if (count >= max) return count;
            }
            found_so_far++;
        }

        bootfs_custom_file_t *cf = (bootfs_custom_file_t*)g_bootfs_state.custom_files;
        while (cf) {
            if (found_so_far >= offset) {
                strcpy(entries[count].name, cf->name);
                entries[count].size = cf->size;
                entries[count].is_directory = 0;
                count++;
                if (count >= max) return count;
            }
            found_so_far++;
            cf = cf->next;
        }
    }
    else if (strcmp(rel_path, "metadata") == 0) {
        const char *meta_files[] = {
            "boot_time",
            "boot_flags",
            "version"
        };
        
        for (int i = 0; i < 3; i++) {
            if (found_so_far >= offset) {
                strcpy(entries[count].name, meta_files[i]);
                entries[count].size = 0;
                entries[count].is_directory = 0;
                count++;
                if (count >= max) return count;
            }
            found_so_far++;
        }
    }
    
    return count;
}

static bool bootfs_mkdir(void *fs_private, const char *rel_path) {
    return false;
}

static bool bootfs_rmdir(void *fs_private, const char *rel_path) {
    return false;
}

static bool bootfs_unlink(void *fs_private, const char *rel_path) {
    if (!rel_path) return false;
    if (rel_path[0] == '/') rel_path++;
    
    if (strncmp(rel_path, "efi/", 4) == 0 || strcmp(rel_path, "efi") == 0) {
        return false;
    }
    
    if (strcmp(rel_path, "kernel") == 0 || strcmp(rel_path, "initrd") == 0 || strcmp(rel_path, "initrd.tar") == 0 || strcmp(rel_path, "metadata") == 0 || is_metadata_file(rel_path)) {
        return false;
    }
    
    char disk_path[256];
    strcpy(disk_path, "/boot/efi/");
    strcat(disk_path, rel_path);
    if (vfs_exists(disk_path)) {
        return vfs_delete(disk_path);
    }
    strcpy(disk_path, "/");
    strcat(disk_path, rel_path);
    if (vfs_exists(disk_path)) {
        return vfs_delete(disk_path);
    }
    return false;
}

static bool bootfs_rename(void *fs_private, const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return false;
    
    const char *old_rel = old_path;
    const char *new_rel = new_path;
    
    if (old_rel[0] == '/') old_rel++;
    if (new_rel[0] == '/') new_rel++;
    
    if (strncmp(old_rel, "efi/", 4) == 0 || strcmp(old_rel, "efi") == 0) {
        return false;
    }
    
    if (strcmp(old_rel, "kernel") == 0 || strcmp(old_rel, "initrd") == 0 || strcmp(old_rel, "initrd.tar") == 0 || strcmp(old_rel, "metadata") == 0 || is_metadata_file(old_rel)) {
        return false;
    }
    
    char old_disk_path[256];
    char new_disk_path[256];
    
    strcpy(old_disk_path, "/boot/efi/");
    strcat(old_disk_path, old_rel);
    if (vfs_exists(old_disk_path)) {
        strcpy(new_disk_path, "/boot/efi/");
        strcat(new_disk_path, new_rel);
        return vfs_rename(old_disk_path, new_disk_path);
    }
    
    strcpy(old_disk_path, "/");
    strcat(old_disk_path, old_rel);
    if (vfs_exists(old_disk_path)) {
        strcpy(new_disk_path, "/");
        strcat(new_disk_path, new_rel);
        return vfs_rename(old_disk_path, new_disk_path);
    }
    return false;
}

static bool bootfs_exists(void *fs_private, const char *rel_path) {
    if (!rel_path) rel_path = "";
    if (rel_path[0] == '/') rel_path++;
    
    if (strncmp(rel_path, "efi/", 4) == 0 || strcmp(rel_path, "efi") == 0) {
        return false;
    }
    
    if (rel_path[0] == '\0') return true;
    
    if (strcmp(rel_path, "kernel") == 0) return true;
    if (strcmp(rel_path, "initrd") == 0) return true;
    if (strcmp(rel_path, "initrd.tar") == 0) return true;
    if (strcmp(rel_path, "metadata") == 0) return true;
    if (is_metadata_file(rel_path)) return true;
    if (bootfs_find_custom(rel_path)) return true;
    
    char disk_path[256];
    strcpy(disk_path, "/boot/efi/");
    strcat(disk_path, rel_path);
    if (vfs_exists(disk_path)) return true;
    strcpy(disk_path, "/");
    strcat(disk_path, rel_path);
    return vfs_exists(disk_path);
}

static bool bootfs_is_dir(void *fs_private, const char *rel_path) {
    if (!rel_path) rel_path = "";
    if (rel_path[0] == '/') rel_path++;
    
    if (rel_path[0] == '\0') return true;
    if (strcmp(rel_path, "efi") == 0) return true;
    if (strcmp(rel_path, "metadata") == 0) return true; 
    
    return false;
}

static int bootfs_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info) {
    if (!info) return -1;
    if (!rel_path) rel_path = "";
    if (rel_path[0] == '/') rel_path++;
    
    if (strncmp(rel_path, "efi/", 4) == 0 || strcmp(rel_path, "efi") == 0) {
        return -1;
    }
    
    memset(info, 0, sizeof(vfs_dirent_t));
    
    if (rel_path[0] == '\0') {
        strcpy(info->name, "/");
        info->is_directory = 1;
        return 0;
    }
    
    if (strcmp(rel_path, "kernel") == 0) {
        strcpy(info->name, "kernel");
        info->size = g_bootfs_state.kernel_size;
        info->is_directory = 0;
        return 0;
    } else if (strcmp(rel_path, "initrd") == 0) {
        strcpy(info->name, "initrd");
        info->size = g_bootfs_state.initrd_size;
        info->is_directory = 0;
        return 0;
    } else if (strcmp(rel_path, "initrd.tar") == 0) {
        strcpy(info->name, "initrd.tar");
        info->size = g_bootfs_state.initrd_size;
        info->is_directory = 0;
        return 0;
    } else if (strcmp(rel_path, "metadata") == 0) {
        strcpy(info->name, "metadata");
        info->is_directory = 1;
        return 0;
    } else if (strcmp(rel_path, "efi") == 0) {
        strcpy(info->name, "efi");
        info->is_directory = 1;
        return 0;
    }
    
    if (is_metadata_file(rel_path)) {
        char temp_buf[4096];
        int len = generate_metadata_content(rel_path, temp_buf, 4096);
        strcpy(info->name, rel_path + 9); 
        info->size = len;
        info->is_directory = 0;
        return 0;
    }
    
    bootfs_custom_file_t *cf = bootfs_find_custom(rel_path);
    if (cf) {
        strcpy(info->name, cf->name);
        info->size = cf->size;
        info->is_directory = 0;
        return 0;
    }
    
    char disk_path[256];
    strcpy(disk_path, "/boot/efi/");
    strcat(disk_path, rel_path);
    vfs_dirent_t v_info;
    if (vfs_get_info(disk_path, &v_info) == 0) {
        strcpy(info->name, rel_path);
        info->size = v_info.size;
        info->is_directory = v_info.is_directory;
        info->write_date = v_info.write_date;
        info->write_time = v_info.write_time;
        return 0;
    }
    
    strcpy(disk_path, "/");
    strcat(disk_path, rel_path);
    if (vfs_get_info(disk_path, &v_info) == 0) {
        strcpy(info->name, rel_path);
        info->size = v_info.size;
        info->is_directory = v_info.is_directory;
        info->write_date = v_info.write_date;
        info->write_time = v_info.write_time;
        return 0;
    }
    
    return -1;
}

static uint32_t bootfs_get_position(void *file_handle) {
    bootfs_handle_t *h = (bootfs_handle_t*)file_handle;
    if (!h) return 0;
    if (h->disk_file) return vfs_file_position(h->disk_file);
    return h->offset;
}

static uint32_t bootfs_get_size(void *file_handle) {
    bootfs_handle_t *h = (bootfs_handle_t*)file_handle;
    if (!h) return 0;
    
    if (h->disk_file) return vfs_file_size(h->disk_file);
    
    if (strcmp(h->path, "kernel") == 0) {
        return g_bootfs_state.kernel_size;
    } else if (strcmp(h->path, "initrd") == 0) {
        return g_bootfs_state.initrd_size;
    } else if (strcmp(h->path, "initrd.tar") == 0) {
        return g_bootfs_state.initrd_size;
    } else if (is_metadata_file(h->path)) {
        char temp_buf[4096];
        return generate_metadata_content(h->path, temp_buf, 4096);
    }
    
    bootfs_custom_file_t *cf = bootfs_find_custom(h->path);
    if (cf) return cf->size;
    
    return 0;
}

vfs_fs_ops_t* bootfs_get_ops(void) {
    return &bootfs_ops;
}

void bootfs_state_init(void) {
    memset(&g_bootfs_state, 0, sizeof(bootfs_state_t));
    
    strcpy(g_bootfs_state.bootloader_name, "Limine");
    strcpy(g_bootfs_state.bootloader_version, "6.0.0");
    

    g_bootfs_state.limine_conf[0] = '\0';
    g_bootfs_state.limine_conf_len = 0;
    
    g_bootfs_state.kernel_size = 0;
    g_bootfs_state.initrd_size = 0;
    g_bootfs_state.boot_time_ms = 0;
}

void bootfs_init(void) {
    bootfs_state_init();
}

void bootfs_mount_boot_partition(void) {
    int count = disk_get_count();
    Disk *esp = NULL;
    
    for (int i = 0; i < count; i++) {
        Disk *d = disk_get_by_index(i);
        if (d && d->is_esp) {
            esp = d;
            break;
        }
    }
    
    if (esp) {
        void *fs_private = fat32_mount_volume(esp);
        if (fs_private) {
            vfs_mount("/boot/efi", esp->devname, "fat32", fat32_get_realfs_ops(), fs_private);
            serial_write("[BOOTFS] Mounted ESP at /boot/efi\n");
        }
    } else {
        serial_write("[BOOTFS] No ESP found for mounting\n");
    }
}

static void find_boot_config(char *out_path) {
    out_path[0] = '\0';
    
    // First try `/boot/efi` directory
    vfs_dirent_t entries[32];
    int count = vfs_list_directory("/boot/efi", entries, 32, 0);
    for (int i = 0; i < count; i++) {
        if (!entries[i].is_directory) {
            size_t len = strlen(entries[i].name);
            if (len > 5 && (strcmp(entries[i].name + len - 5, ".conf") == 0 || strcmp(entries[i].name + len - 4, ".cfg") == 0)) {
                strcpy(out_path, "/boot/efi/");
                strcat(out_path, entries[i].name);
                return;
            }
        }
    }
    
    // If not found, try root `/`
    count = vfs_list_directory("/", entries, 32, 0);
    for (int i = 0; i < count; i++) {
        if (!entries[i].is_directory) {
            size_t len = strlen(entries[i].name);
            if (len > 5 && (strcmp(entries[i].name + len - 5, ".conf") == 0 || strcmp(entries[i].name + len - 4, ".cfg") == 0)) {
                strcpy(out_path, "/");
                strcat(out_path, entries[i].name);
                return;
            }
        }
    }
}

void bootfs_refresh_from_disk(void) {
    extern vfs_file_t* vfs_open(const char *path, const char *mode);
    extern int vfs_read(vfs_file_t *file, void *buf, size_t size);
    extern void vfs_close(vfs_file_t *file);
    
    char path[128];
    find_boot_config(path);
    if (path[0] != '\0') {
        strcpy(g_limine_conf_path, path);
        vfs_file_t *boot_conf = vfs_open(path, "r");
        if (boot_conf) {
            int bytes_read = vfs_read(boot_conf, g_bootfs_state.limine_conf, 2047);
            if (bytes_read > 0) {
                g_bootfs_state.limine_conf[bytes_read] = '\0';
                g_bootfs_state.limine_conf_len = bytes_read;
                serial_write("[BOOTFS] Loaded boot configuration from ");
                serial_write(g_limine_conf_path);
                serial_write("\n");
            }
            vfs_close(boot_conf);
        }
    } else {
        serial_write("[BOOTFS] Warning: no boot config file (*.conf/*.cfg) found on disk\n");
    }
}
