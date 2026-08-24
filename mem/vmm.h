// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_VMM_H
#define BOREDOS_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "vma.h"
#include "mmu.h"
#include "spinlock.h"
#include "wait_queue.h"

struct registers_t;

typedef struct {
    spinlock_t lock;
    int32_t count;
    uint32_t wait_readers;
    uint32_t wait_writers;
    wait_queue_head_t wait_q;
} vmm_rwsem_t;

typedef struct vmm_space {
    mmu_context_t *mmu_ctx;
    vm_area_t *vma_head;
    rb_root_t vma_tree;
    vmm_rwsem_t mmap_sem;
    _Atomic uint64_t active_cpus;
    _Atomic uint32_t refcount;
    uintptr_t mmap_base;
    uintptr_t mmap_end;
    uintptr_t brk_start;
    uintptr_t brk_current;
} vmm_space_t;

void vmm_rwsem_init(vmm_rwsem_t *sem);
void vmm_down_read(vmm_rwsem_t *sem);
void vmm_up_read(vmm_rwsem_t *sem);
void vmm_down_write(vmm_rwsem_t *sem);
void vmm_up_write(vmm_rwsem_t *sem);

void vmm_init(void);
vmm_space_t *vmm_get_kernel_space(void);
vmm_space_t *vmm_create_space(void);
void vmm_destroy_space(vmm_space_t *space);
vmm_space_t *vmm_clone_space(vmm_space_t *parent_space);

void *vmm_map(vmm_space_t *space, uintptr_t hint, size_t length, uint32_t prot, uint32_t flags, void *file, uint64_t offset);
int vmm_unmap(vmm_space_t *space, uintptr_t addr, size_t length);
int vmm_protect(vmm_space_t *space, uintptr_t addr, size_t length, uint32_t prot);
uintptr_t vmm_brk(vmm_space_t *space, uintptr_t new_brk);

int vmm_handle_page_fault(vmm_space_t *space, uintptr_t fault_addr, uint32_t error_code, struct registers_t *regs);

struct page *vmm_get_zero_page(void);
uintptr_t vmm_get_zero_paddr(void);

void *vmalloc(size_t size);
void vfree(void *addr);

#endif
 // BOREDOS_VMM_H
