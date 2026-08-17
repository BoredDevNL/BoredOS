// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "mmu.h"
#include <stddef.h>

void mmu_init(void) {
}

mmu_context_t mmu_create_context(void) {
    return 0;
}

void mmu_destroy_context(mmu_context_t ctx) {
    (void)ctx;
}

void mmu_switch_context(mmu_context_t ctx) {
    (void)ctx;
}

mmu_context_t mmu_get_current_context(void) {
    return 0;
}

mmu_context_t mmu_get_kernel_context(void) {
    return 0;
}

bool mmu_map_page(mmu_context_t ctx, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    (void)ctx; (void)virt; (void)phys; (void)flags;
    return true;
}

bool mmu_map_pages(mmu_context_t ctx, uintptr_t virt, uintptr_t phys, size_t count, uint32_t flags) {
    (void)ctx; (void)virt; (void)phys; (void)count; (void)flags;
    return true;
}

void mmu_unmap_page(mmu_context_t ctx, uintptr_t virt) {
    (void)ctx; (void)virt;
}

void mmu_unmap_pages(mmu_context_t ctx, uintptr_t virt, size_t count) {
    (void)ctx; (void)virt; (void)count;
}

bool mmu_protect_page(mmu_context_t ctx, uintptr_t virt, uint32_t flags) {
    (void)ctx; (void)virt; (void)flags;
    return true;
}

uintptr_t mmu_virt_to_phys(mmu_context_t ctx, uintptr_t virt) {
    (void)ctx; (void)virt;
    return 0;
}

void mmu_tlb_flush_page(uintptr_t virt) {
    (void)virt;
}

void mmu_tlb_flush_all(void) {
}

void mmu_tlb_shootdown(uintptr_t virt, size_t count) {
    (void)virt; (void)count;
}
