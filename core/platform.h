// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

typedef struct {
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t microcode;
    uint64_t flags;
    uint32_t cache_size;
} cpu_info_t;

void platform_init(void);
uint64_t p2v(uint64_t phys);
uint64_t v2p(uint64_t virt);
void platform_get_cpu_model(char *model);
void platform_get_cpu_vendor(char *vendor);
void platform_get_cpu_info(cpu_info_t *info);
void platform_get_cpu_flags(char *flags_str);

#endif
