// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "tar.h"
#include "vfs.h"

// The standard TAR header block is 512 bytes.
struct tar_header {
    char filename[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

// Helper: parse tar octal field representation
static uint64_t tar_parse_octal(const char *str, int size) {
    uint64_t result = 0;
    while (size-- > 0) {
        if (*str >= '0' && *str <= '7') {
            result = (result << 3) + (*str - '0');
        }
        str++;
    }
    return result;
}

// VFS helper: recursively create directories
static void vfs_mkdir_recursive(const char *path) {
    char temp[256];
    int len = 0;
    while (path[len] && len < 255) {
        temp[len] = path[len];
        if (temp[len] == '/' && len > 0) {
            temp[len] = '\0';
            if (!vfs_exists(temp)) {
                vfs_mkdir(temp);
            }
            temp[len] = '/';
        }
        len++;
    }
    temp[len] = '\0';
    if (!vfs_exists(temp)) {
        vfs_mkdir(temp);
    }
}

void tar_parse(void *archive, uint64_t archive_size) {
    uint8_t *ptr = (uint8_t *)archive;
    uint8_t *end = ptr + archive_size;

    while (ptr + 512 <= end) {
        struct tar_header *header = (struct tar_header *)ptr;

        // End of archive is marked by empty blocks
        if (header->filename[0] == '\0') {
            break;
        }

        uint64_t file_size = tar_parse_octal(header->size, 11);
        
        char full_path[256];
        // Ensure path starts with a '/' for VFS consistency
        if (header->filename[0] != '/') {
            full_path[0] = '/';
            int j = 0;
            while (header->filename[j] && j < 254) {
                full_path[j + 1] = header->filename[j];
                j++;
            }
            full_path[j + 1] = '\0';
        } else {
            int j = 0;
            while (header->filename[j] && j < 255) {
                full_path[j] = header->filename[j];
                j++;
            }
            full_path[j] = '\0';
        }

        if (header->typeflag == '5') {
            // It's a directory
            vfs_mkdir_recursive(full_path);
        } else if (header->typeflag == '0' || header->typeflag == '\0') {
            // It's a normal file
            // First ensure the parent directory exists
            char parent_path[256];
            int last_slash = -1;
            for (int j = 0; full_path[j]; j++) {
                parent_path[j] = full_path[j];
                if (full_path[j] == '/') {
                    last_slash = j;
                }
            }
            if (last_slash > 0) {
                parent_path[last_slash] = '\0';
                vfs_mkdir_recursive(parent_path);
            }
            
            vfs_file_t *fh = vfs_open(full_path, "w");
            if (fh) {
                vfs_write(fh, ptr + 512, (int)file_size);
                vfs_close(fh);
            }
        }
        
        // Advance pointer to the next file header
        // Header block (512) + File data (padded to 512-byte multiples)
        uint64_t data_blocks = (file_size + 511) / 512;
        ptr += 512 + (data_blocks * 512);
    }
}
