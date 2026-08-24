// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.


#ifndef BOREDOS_MMU_H
#define BOREDOS_MMU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "spinlock.h"

#define MMU_PROT_READ    (1 << 0)
#define MMU_PROT_WRITE   (1 << 1)
#define MMU_PROT_EXEC    (1 << 2)
#define MMU_PROT_USER    (1 << 3)
#define MMU_FLAG_NOCACHE (1 << 4)
#define MMU_FLAG_WC      (1 << 5)
#define MMU_FLAG_GLOBAL  (1 << 6)
#define MMU_FLAG_HUGE_2M (1 << 7)
#define MMU_FLAG_COW     (1 << 8)

typedef struct mmu_context {
    uintptr_t pml4_phys;
    spinlock_t lock;
} mmu_context_t;

void mmu_init(void);

mmu_context_t *mmu_create_context(void);
void mmu_destroy_context(mmu_context_t *ctx);
void mmu_switch_context(mmu_context_t *ctx);
mmu_context_t *mmu_get_current_context(void);
mmu_context_t *mmu_get_kernel_context(void);

int mmu_map_page(mmu_context_t *ctx, uintptr_t virt, uintptr_t phys, uint32_t flags);
int mmu_map_pages(mmu_context_t *ctx, uintptr_t virt, uintptr_t phys, size_t count, uint32_t flags);
int mmu_unmap_page(mmu_context_t *ctx, uintptr_t virt);
int mmu_unmap_pages(mmu_context_t *ctx, uintptr_t virt, size_t count);
int mmu_protect_page(mmu_context_t *ctx, uintptr_t virt, uint32_t flags);
uintptr_t mmu_virt_to_phys(mmu_context_t *ctx, uintptr_t virt);
uint64_t *mmu_get_pte_ptr(mmu_context_t *ctx, uintptr_t virt);
int mmu_clone_user_cow(mmu_context_t *parent_ctx, mmu_context_t *child_ctx);

void mmu_tlb_flush_page(uintptr_t virt);
void mmu_tlb_flush_all(void);
void mmu_tlb_shootdown(uintptr_t virt, size_t count);
void mmu_tlb_shootdown_target(uint64_t target_cpus, uintptr_t virt, size_t count);
void mmu_tlb_ipi_handler(void);

#endif // BOREDOS_MMU_H
