// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
#ifndef BOOTFS_STATE_H
#define BOOTFS_STATE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char bootloader_name[64];
    char bootloader_version[64];
    uint64_t boot_time_ms;
    uint8_t boot_flags;
    char root_device[16];
    char limine_conf[2048];
    int limine_conf_len;
    uint32_t num_modules;
    uint32_t kernel_size;
    uint32_t initrd_size;
    void *initrd_ptr;
    void *custom_files;
} bootfs_state_t;

#define BOOT_FLAG_LIVE          0x01
#define BOOT_FLAG_DISK          0x02
#define BOOT_FLAG_FORCED        0x04
#define BOOT_FLAG_ROOT_SET      0x08
#define BOOT_FLAG_ROOT_PIVOTED  0x10
extern bootfs_state_t g_bootfs_state;
void bootfs_state_init(void);

#endif
