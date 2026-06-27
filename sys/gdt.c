// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "gdt.h"
#include <stdint.h>
#include <stddef.h>
#include "memory_manager.h"

static void *gdt_memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) {
        *p++ = (uint8_t)c;
    }
    return s;
}

// Base GDT: 5 segments + 1 TSS (2 entries) = 7 entries for BSP.
// For SMP: we add 2 entries per additional CPU for their TSS.
// Max supported: 32 CPUs → 5 + 2*32 = 69 entries max.
#define GDT_BASE_ENTRIES 5    // NULL, KCode, KData, UData, UCode
#define GDT_MAX_ENTRIES 69    // 5 + 2*32

struct gdt_entry gdt[GDT_MAX_ENTRIES];
struct gdt_ptr gdtr;
struct tss_entry tss;  // BSP TSS (CPU 0)

// Per-CPU TSS array (dynamically allocated for AP cores)
static struct tss_entry *ap_tss_array = NULL;
static uint32_t ap_tss_count = 0;

extern void gdt_flush(uint64_t);
extern void tss_flush(void);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access = access;
}

// Write a 16-byte TSS descriptor into GDT entries [num] and [num+1]
static void gdt_set_tss_gate_at(int num, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    struct {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  granularity;
        uint8_t  base_high;
        uint32_t base_upper;
        uint32_t reserved;
    } __attribute__((packed)) *tss_desc = (void*)&gdt[num];

    tss_desc->base_low = (base & 0xFFFF);
    tss_desc->base_middle = (base >> 16) & 0xFF;
    tss_desc->base_high = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32);

    tss_desc->limit_low = (limit & 0xFFFF);
    tss_desc->granularity = ((limit >> 16) & 0x0F);

    tss_desc->granularity |= (gran & 0xF0);
    tss_desc->access = access;
    tss_desc->reserved = 0;
}

void tss_set_stack(uint64_t kernel_stack) {
    tss.rsp0 = kernel_stack;
}

void tss_set_stack_cpu(uint32_t cpu_id, uint64_t kernel_stack) {
    if (cpu_id == 0) {
        tss.rsp0 = kernel_stack;
    } else if (ap_tss_array && cpu_id - 1 < ap_tss_count) {
        ap_tss_array[cpu_id - 1].rsp0 = kernel_stack;
    }
}

void gdt_init(void) {
    // Start with 7 entries (5 segments + BSP TSS taking 2)
    gdtr.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gdtr.base = (uint64_t)&gdt;

    // NULL segment
    gdt_set_gate(0, 0, 0, 0, 0);

    // Kernel Code segment (Ring 0, 64-bit)
    gdt_set_gate(1, 0, 0, 0x9A, 0xAF);

    // Kernel Data segment (Ring 0)
    gdt_set_gate(2, 0, 0, 0x92, 0xAF);

    // User Data segment (Ring 3)
    gdt_set_gate(3, 0, 0, 0xF2, 0xAF);

    // User Code segment (Ring 3, 64-bit)
    gdt_set_gate(4, 0, 0, 0xFA, 0xAF);

    // BSP TSS segment (entries 5 and 6)
    gdt_memset(&tss, 0, sizeof(struct tss_entry));
    tss.iopb_offset = sizeof(struct tss_entry);
    
    void* initial_tss_stack = kmalloc_aligned(4096, 4096);
    if (initial_tss_stack) {
        tss.rsp0 = (uint64_t)initial_tss_stack + 4096;
    }
    
    gdt_set_tss_gate_at(5, (uint64_t)&tss, sizeof(struct tss_entry) - 1, 0x89, 0x00);

    gdt_flush((uint64_t)&gdtr);
    tss_flush();
}

// SMP: Add TSS entries for all AP cores and reload the GDT.
void gdt_init_ap_tss(uint32_t cpu_count) {
    if (cpu_count <= 1) return; // No APs

    uint32_t ap_count = cpu_count - 1;
    ap_tss_count = ap_count;

    // Allocate per-CPU TSS structures
    ap_tss_array = (struct tss_entry *)kmalloc(ap_count * sizeof(struct tss_entry));
    if (!ap_tss_array) return;
    gdt_memset(ap_tss_array, 0, ap_count * sizeof(struct tss_entry));

    // Each AP TSS goes at GDT slot 7 + (i*2) (since slot 5-6 is BSP TSS)
    for (uint32_t i = 0; i < ap_count; i++) {
        int gdt_slot = 7 + (i * 2);
        if (gdt_slot + 1 >= GDT_MAX_ENTRIES) break;

        ap_tss_array[i].iopb_offset = sizeof(struct tss_entry);

        // Allocate a kernel stack for this AP's interrupt handling
        void *ap_int_stack = kmalloc_aligned(8192, 4096);
        if (ap_int_stack) {
            ap_tss_array[i].rsp0 = (uint64_t)ap_int_stack + 8192;
        }

        gdt_set_tss_gate_at(gdt_slot, (uint64_t)&ap_tss_array[i],
                            sizeof(struct tss_entry) - 1, 0x89, 0x00);
    }

    // Update GDT limit to include all new entries
    uint32_t total_entries = 7 + (ap_count * 2);
    if (total_entries > GDT_MAX_ENTRIES) total_entries = GDT_MAX_ENTRIES;
    gdtr.limit = (sizeof(struct gdt_entry) * total_entries) - 1;

    // Reload GDTR on BSP with the expanded limit.
    // We must NOT call tss_flush() here — the BSP TSS is already loaded
    // and marked "busy" (0x8B). Trying to LTR a busy TSS causes GPF.
    asm volatile("lgdt %0" : : "m"(gdtr));
}

// SMP: Load the TSS for a specific AP. Called from ap_entry().
void gdt_load_ap_tss(uint32_t cpu_id) {
    if (cpu_id == 0) {
        // BSP uses slot 5 → selector 0x28
        asm volatile("mov $0x28, %%ax; ltr %%ax" ::: "ax");
        return;
    }

    // AP cpu_id maps to GDT slot 7 + ((cpu_id-1) * 2)
    uint16_t selector = (uint16_t)((7 + ((cpu_id - 1) * 2)) * sizeof(struct gdt_entry));
    asm volatile("ltr %0" : : "r"(selector));
}
