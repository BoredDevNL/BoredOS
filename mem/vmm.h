// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
/*
################################################################

BEWARE

THIS IS SCAFFOLDING, CAN VERY MUCH BE CHANGED!!

#################################################################

*/

#ifndef BOREDOS_VMM_H
#define BOREDOS_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vma.h"
#include "mmu.h"
#include "spinlock.h"

typedef struct vmm_space {
    mmu_context_t *mmu_ctx;
    vm_area_t *vma_head;
    vm_area_t *vma_root;
    spinlock_t lock;
    uint32_t refcount;
    uintptr_t mmap_base;
    uintptr_t mmap_end;
} vmm_space_t;

void vmm_init(void);
vmm_space_t *vmm_get_kernel_space(void);
vmm_space_t *vmm_create_space(void);
void vmm_destroy_space(vmm_space_t *space);

vmm_space_t *vmm_clone_space(vmm_space_t *parent_space);

void *vmm_map(vmm_space_t *space, uintptr_t hint, size_t length, uint32_t prot, uint32_t flags, void *file, uint64_t offset);
int vmm_unmap(vmm_space_t *space, uintptr_t addr, size_t length);
int vmm_protect(vmm_space_t *space, uintptr_t addr, size_t length, uint32_t prot);

int vmm_handle_page_fault(vmm_space_t *space, uintptr_t fault_addr, uint32_t error_code);

#endif // BOREDOS_VMM_H
