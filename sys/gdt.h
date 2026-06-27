// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Segment Selectors
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS   0x1B // 0x18 | 3 (RPL 3)
#define USER_DS   0x23 // 0x20 | 3 (RPL 3)
#define TSS_SEG   0x28

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);
void tss_set_stack(uint64_t kernel_stack);

// SMP: Initialize per-CPU TSS entries. Call after smp detects cpu_count.
void gdt_init_ap_tss(uint32_t cpu_count);
// SMP: Load the TSS for a specific CPU (called from AP entry).
void gdt_load_ap_tss(uint32_t cpu_id);
// SMP: Set kernel stack for a specific CPU's TSS.
void tss_set_stack_cpu(uint32_t cpu_id, uint64_t kernel_stack);

#endif
