// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "vmm.h"
#include "pmm.h"
#include "slab.h"
#include "platform.h"
#include "smp.h"
#include "process.h"
#include <string.h>

#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17
#define EACCES 13
#define EFAULT 14

#define PAGE_SIZE 4096UL
#define PAGE_MASK (PAGE_SIZE - 1)

#define USER_MMAP_BASE 0x0000700000000000ULL
#define USER_MMAP_END  0x00007FFFFFF00000ULL
#define MAX_STACK_SIZE (8UL * 1024 * 1024)

#define VMALLOC_BASE   0xFFFFC00000000000ULL
#define VMALLOC_END    0xFFFFD00000000000ULL

#define PT_PRESENT     (1ULL << 0)
#define PT_RW          (1ULL << 1)
#define PT_USER        (1ULL << 2)
#define PT_ACCESSED    (1ULL << 5)
#define PT_DIRTY       (1ULL << 6)
#define PT_COW         (1ULL << 9)
#define PT_NX          (1ULL << 63)
#define PT_ADDR_MASK   0x000FFFFFFFFFF000ULL

static vmm_space_t kernel_space;
static uintptr_t zero_page_paddr = 0;
static page_t *zero_page_desc = NULL;

static spinlock_t vmalloc_lock = SPINLOCK_INIT;
static uintptr_t vmalloc_current = VMALLOC_BASE;

void vmm_rwsem_init(vmm_rwsem_t *sem) {
    if (!sem) return;
    sem->lock = SPINLOCK_INIT;
    sem->count = 0;
    sem->wait_readers = 0;
    sem->wait_writers = 0;
    wait_queue_init(&sem->wait_q);
}

void vmm_down_read(vmm_rwsem_t *sem) {
    if (!sem) return;
    while (true) {
        uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
        if (sem->count >= 0 && sem->wait_writers == 0) {
            sem->count++;
            spinlock_release_irqrestore(&sem->lock, flags);
            return;
        }
        sem->wait_readers++;
        spinlock_release_irqrestore(&sem->lock, flags);

        wait_queue_wait(&sem->wait_q);

        flags = spinlock_acquire_irqsave(&sem->lock);
        sem->wait_readers--;
        spinlock_release_irqrestore(&sem->lock, flags);
    }
}

void vmm_up_read(vmm_rwsem_t *sem) {
    if (!sem) return;
    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        if (sem->count == 0 && sem->wait_writers > 0) {
            wait_queue_wake_all(&sem->wait_q);
        }
    }
    spinlock_release_irqrestore(&sem->lock, flags);
}

void vmm_down_write(vmm_rwsem_t *sem) {
    if (!sem) return;
    while (true) {
        uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
        if (sem->count == 0) {
            sem->count = -1;
            spinlock_release_irqrestore(&sem->lock, flags);
            return;
        }
        sem->wait_writers++;
        spinlock_release_irqrestore(&sem->lock, flags);

        wait_queue_wait(&sem->wait_q);

        flags = spinlock_acquire_irqsave(&sem->lock);
        sem->wait_writers--;
        spinlock_release_irqrestore(&sem->lock, flags);
    }
}

void vmm_up_write(vmm_rwsem_t *sem) {
    if (!sem) return;
    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    sem->count = 0;
    if (sem->wait_writers > 0 || sem->wait_readers > 0) {
        wait_queue_wake_all(&sem->wait_q);
    }
    spinlock_release_irqrestore(&sem->lock, flags);
}

void vmm_init(void) {
    zero_page_desc = pmm_alloc_page(PAGE_FLAG_KERNEL | PAGE_FLAG_ZERO);
    if (zero_page_desc) {
        zero_page_paddr = pmm_page_to_paddr(zero_page_desc);
    }

    memset(&kernel_space, 0, sizeof(vmm_space_t));
    kernel_space.mmu_ctx = mmu_get_kernel_context();
    vmm_rwsem_init(&kernel_space.mmap_sem);
    kernel_space.refcount = 1;
    kernel_space.mmap_base = VMALLOC_BASE;
    kernel_space.mmap_end = VMALLOC_END;
}

vmm_space_t *vmm_get_kernel_space(void) {
    return &kernel_space;
}

page_t *vmm_get_zero_page(void) {
    return zero_page_desc;
}

uintptr_t vmm_get_zero_paddr(void) {
    return zero_page_paddr;
}

vmm_space_t *vmm_create_space(void) {
    vmm_space_t *space = (vmm_space_t *)kmalloc(sizeof(vmm_space_t));
    if (!space) return NULL;

    memset(space, 0, sizeof(vmm_space_t));
    space->mmu_ctx = mmu_create_context();
    if (!space->mmu_ctx) {
        kfree(space);
        return NULL;
    }

    vmm_rwsem_init(&space->mmap_sem);
    space->refcount = 1;
    space->active_cpus = 0;
    space->mmap_base = USER_MMAP_BASE;
    space->mmap_end = USER_MMAP_END;
    space->vma_tree = RB_ROOT;
    return space;
}

void vmm_destroy_space(vmm_space_t *space) {
    if (!space || space == &kernel_space) return;

    if (__atomic_sub_fetch(&space->refcount, 1, __ATOMIC_SEQ_CST) > 0) {
        return;
    }

    vmm_down_write(&space->mmap_sem);

    vm_area_t *curr = space->vma_head;
    while (curr) {
        vm_area_t *next = curr->next;
        if (curr->type == VMA_TYPE_ANON && !(curr->flags & VMA_FLAG_SHARED)) {
            size_t page_count = (curr->end - curr->start) >> 12;
            for (size_t i = 0; i < page_count; i++) {
                uintptr_t v = curr->start + (i << 12);
                uintptr_t p = mmu_virt_to_phys(space->mmu_ctx, v);
                if (p) {
                    page_t *pg = pmm_paddr_to_page(p);
                    if (pg && pg != zero_page_desc && !(pg->flags & (PAGE_FLAG_FREE | PAGE_FLAG_SLAB | PAGE_FLAG_KMALLOC_LARGE))) {
                        pmm_free_page(pg);
                    }
                }
            }
        }
        vma_destroy(curr);
        curr = next;
    }
    space->vma_head = NULL;
    space->vma_tree = RB_ROOT;

    vmm_up_write(&space->mmap_sem);

    if (space->mmu_ctx) {
        mmu_destroy_context(space->mmu_ctx);
    }

    kfree(space);
}

vmm_space_t *vmm_clone_space(vmm_space_t *parent_space) {
    if (!parent_space) return NULL;

    vmm_space_t *child = vmm_create_space();
    if (!child) return NULL;

    vmm_down_write(&parent_space->mmap_sem);

    child->mmap_base = parent_space->mmap_base;
    child->mmap_end = parent_space->mmap_end;
    child->brk_start = parent_space->brk_start;
    child->brk_current = parent_space->brk_current;

    vm_area_t *curr = parent_space->vma_head;
    while (curr) {
        uint32_t flags = curr->flags;
        if ((flags & VMA_FLAG_WRITE) && !(flags & VMA_FLAG_SHARED)) {
            flags |= VMA_FLAG_COW;
            curr->flags |= VMA_FLAG_COW;
        }

        vm_area_t *clone = vma_create(curr->start, curr->end, curr->type, flags);
        if (clone) {
            clone->backing_file = curr->backing_file;
            clone->file_offset = curr->file_offset;
            vma_insert(&child->vma_head, &child->vma_tree, clone);
        }
        curr = curr->next;
    }

    mmu_clone_user_cow(parent_space->mmu_ctx, child->mmu_ctx);

    vmm_up_write(&parent_space->mmap_sem);
    return child;
}

void *vmm_map(vmm_space_t *space, uintptr_t hint, size_t length, uint32_t prot, uint32_t flags, void *file, uint64_t offset) {
    if (!space || length == 0) return NULL;

    size_t aligned_len = (length + PAGE_MASK) & ~PAGE_MASK;
    uintptr_t start = hint;

    vmm_down_write(&space->mmap_sem);

    if (start == 0) {
        start = vma_find_unmapped_area(&space->vma_tree, space->mmap_base, space->mmap_end, aligned_len, PAGE_SIZE);
        if (start == 0) {
            vmm_up_write(&space->mmap_sem);
            return NULL;
        }
    } else {
        if (start < 0x10000 || start + aligned_len > space->mmap_end) {
            vmm_up_write(&space->mmap_sem);
            return NULL;
        }
    }

    uint32_t vma_flags = 0;
    if (prot & MMU_PROT_READ)  vma_flags |= VMA_FLAG_READ;
    if (prot & MMU_PROT_WRITE) vma_flags |= VMA_FLAG_WRITE;
    if (prot & MMU_PROT_EXEC)  vma_flags |= VMA_FLAG_EXEC;
    if (flags & MMU_FLAG_COW)  vma_flags |= VMA_FLAG_COW;
    if (flags & VMA_FLAG_SHARED) vma_flags |= VMA_FLAG_SHARED;

    uint32_t vma_type = file ? ((flags & VMA_FLAG_SHARED) ? VMA_TYPE_SHM : VMA_TYPE_FILE) : VMA_TYPE_ANON;
    vm_area_t *vma = vma_create(start, start + aligned_len, vma_type, vma_flags);
    if (!vma) {
        vmm_up_write(&space->mmap_sem);
        return NULL;
    }

    vma->backing_file = file;
    vma->file_offset = offset;

    if (!vma_insert(&space->vma_head, &space->vma_tree, vma)) {
        vma_destroy(vma);
        vmm_up_write(&space->mmap_sem);
        return NULL;
    }

    vmm_up_write(&space->mmap_sem);
    return (void *)start;
}

int vmm_unmap(vmm_space_t *space, uintptr_t addr, size_t length) {
    if (!space || length == 0 || (addr & PAGE_MASK)) return -EINVAL;

    size_t aligned_len = (length + PAGE_MASK) & ~PAGE_MASK;
    uintptr_t end = addr + aligned_len;

    vmm_down_write(&space->mmap_sem);

    vm_area_t *curr = space->vma_head;
    while (curr) {
        vm_area_t *next = curr->next;
        if (curr->start >= end || curr->end <= addr) {
            curr = next;
            continue;
        }

        bool is_private_anon = (curr->type == VMA_TYPE_ANON) && !(curr->flags & VMA_FLAG_SHARED);
        uintptr_t unmap_start = (curr->start > addr) ? curr->start : addr;
        uintptr_t unmap_end = (curr->end < end) ? curr->end : end;

        if (is_private_anon) {
            size_t pages = (unmap_end - unmap_start) >> 12;
            for (size_t i = 0; i < pages; i++) {
                uintptr_t v = unmap_start + (i << 12);
                uintptr_t p = mmu_virt_to_phys(space->mmu_ctx, v);
                if (p) {
                    page_t *pg = pmm_paddr_to_page(p);
                    if (pg && pg != zero_page_desc && !(pg->flags & (PAGE_FLAG_FREE | PAGE_FLAG_SLAB | PAGE_FLAG_KMALLOC_LARGE))) {
                        pmm_free_page(pg);
                    }
                }
            }
        }

        if (curr->start >= addr && curr->end <= end) {
            vma_remove(&space->vma_head, &space->vma_tree, curr);
            vma_destroy(curr);
        } else if (curr->start < addr && curr->end > end) {
            vm_area_t *right = vma_create(end, curr->end, curr->type, curr->flags);
            if (right) {
                right->backing_file = curr->backing_file;
                if (curr->type == VMA_TYPE_FILE) {
                    right->file_offset = curr->file_offset + (end - curr->start);
                }
                vma_remove(&space->vma_head, &space->vma_tree, curr);
                curr->end = addr;
                vma_insert(&space->vma_head, &space->vma_tree, curr);
                vma_insert(&space->vma_head, &space->vma_tree, right);
            }
        } else if (curr->start < addr && curr->end <= end) {
            vma_remove(&space->vma_head, &space->vma_tree, curr);
            curr->end = addr;
            vma_insert(&space->vma_head, &space->vma_tree, curr);
        } else if (curr->start >= addr && curr->end > end) {
            vma_remove(&space->vma_head, &space->vma_tree, curr);
            if (curr->type == VMA_TYPE_FILE) {
                curr->file_offset += (end - curr->start);
            }
            curr->start = end;
            vma_insert(&space->vma_head, &space->vma_tree, curr);
        }
        curr = next;
    }

    size_t page_count = aligned_len >> 12;
    mmu_unmap_pages(space->mmu_ctx, addr, page_count);
    mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), addr, page_count);

    vmm_up_write(&space->mmap_sem);
    return 0;
}

int vmm_protect(vmm_space_t *space, uintptr_t addr, size_t length, uint32_t prot) {
    if (!space || length == 0 || (addr & PAGE_MASK)) return -EINVAL;

    size_t aligned_len = (length + PAGE_MASK) & ~PAGE_MASK;
    uintptr_t end = addr + aligned_len;

    vmm_down_write(&space->mmap_sem);

    uint32_t vma_flags = 0;
    if (prot & MMU_PROT_READ)  vma_flags |= VMA_FLAG_READ;
    if (prot & MMU_PROT_WRITE) vma_flags |= VMA_FLAG_WRITE;
    if (prot & MMU_PROT_EXEC)  vma_flags |= VMA_FLAG_EXEC;

    vm_area_t *curr = space->vma_head;
    while (curr) {
        if (curr->start < end && curr->end > addr) {
            curr->flags = (curr->flags & ~(VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC)) | vma_flags;
        }
        curr = curr->next;
    }

    size_t count = aligned_len >> 12;
    for (size_t i = 0; i < count; i++) {
        mmu_protect_page(space->mmu_ctx, addr + (i << 12), prot | MMU_PROT_USER);
    }

    mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), addr, count);

    vmm_up_write(&space->mmap_sem);
    return 0;
}

uintptr_t vmm_brk(vmm_space_t *space, uintptr_t new_brk) {
    if (!space) return 0;

    vmm_down_write(&space->mmap_sem);

    if (new_brk == 0 || new_brk == space->brk_current) {
        uintptr_t ret = space->brk_current;
        vmm_up_write(&space->mmap_sem);
        return ret;
    }

    if (space->brk_start == 0) {
        space->brk_start = 0x20000000;
        space->brk_current = 0x20000000;
    }

    if (new_brk < space->brk_start) {
        uintptr_t ret = space->brk_current;
        vmm_up_write(&space->mmap_sem);
        return ret;
    }

    vm_area_t *heap_vma = NULL;
    for (vm_area_t *c = space->vma_head; c; c = c->next) {
        if (c->flags & VMA_FLAG_HEAP) {
            heap_vma = c;
            break;
        }
    }

    uintptr_t new_aligned = (new_brk + PAGE_MASK) & ~PAGE_MASK;

    if (!heap_vma) {
        if (new_aligned > space->brk_start) {
            heap_vma = vma_create(space->brk_start, new_aligned, VMA_TYPE_ANON,
                                  VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_HEAP);
            if (heap_vma) {
                vma_insert(&space->vma_head, &space->vma_tree, heap_vma);
            }
        }
        space->brk_current = new_brk;
        vmm_up_write(&space->mmap_sem);
        return space->brk_current;
    }

    if (new_brk > space->brk_current) {
        vma_remove(&space->vma_head, &space->vma_tree, heap_vma);
        heap_vma->end = new_aligned;
        vma_insert(&space->vma_head, &space->vma_tree, heap_vma);
        space->brk_current = new_brk;
    } else {
        uintptr_t old_aligned = (space->brk_current + PAGE_MASK) & ~PAGE_MASK;
        if (new_aligned < old_aligned) {
            size_t pages_to_free = (old_aligned - new_aligned) >> 12;
            for (size_t i = 0; i < pages_to_free; i++) {
                uintptr_t v = new_aligned + (i << 12);
                uintptr_t p = mmu_virt_to_phys(space->mmu_ctx, v);
                if (p) {
                    page_t *pg = pmm_paddr_to_page(p);
                    if (pg && pg != zero_page_desc) {
                        pmm_free_page(pg);
                    }
                }
            }
            mmu_unmap_pages(space->mmu_ctx, new_aligned, pages_to_free);
            mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), new_aligned, pages_to_free);
        }
        vma_remove(&space->vma_head, &space->vma_tree, heap_vma);
        if (new_aligned > heap_vma->start) {
            heap_vma->end = new_aligned;
            vma_insert(&space->vma_head, &space->vma_tree, heap_vma);
        } else {
            vma_destroy(heap_vma);
        }
        space->brk_current = new_brk;
    }

    uintptr_t ret = space->brk_current;
    vmm_up_write(&space->mmap_sem);
    return ret;
}

int vmm_handle_page_fault(vmm_space_t *space, uintptr_t fault_addr, uint32_t error_code, registers_t *regs) {
    if (!space || fault_addr >= 0x8000000000000000ULL) return -EFAULT;

    uintptr_t page_vaddr = fault_addr & ~PAGE_MASK;

    vmm_down_read(&space->mmap_sem);

    vm_area_t *vma = vma_find(&space->vma_tree, fault_addr);
    if (!vma) {
        vm_area_t *next_vma = NULL;
        for (vm_area_t *c = space->vma_head; c; c = c->next) {
            if (c->start > fault_addr) {
                next_vma = c;
                break;
            }
        }

        if (next_vma && (next_vma->flags & VMA_FLAG_STACK)) {
            if (regs && regs->rsp < 0xFFFF800000000000ULL) {
                uint64_t rsp = regs->rsp;
                if (fault_addr < rsp - 128 && fault_addr + 4096 < rsp) {
                    vmm_up_read(&space->mmap_sem);
                    return -EACCES;
                }
            }

            if (next_vma->end - page_vaddr > MAX_STACK_SIZE) {
                vmm_up_read(&space->mmap_sem);
                return -EACCES;
            }

            vmm_up_read(&space->mmap_sem);
            vmm_down_write(&space->mmap_sem);

            vm_area_t *stack_target = NULL;
            for (vm_area_t *c = space->vma_head; c; c = c->next) {
                if ((c->flags & VMA_FLAG_STACK) && c->start > fault_addr) {
                    stack_target = c;
                    break;
                }
            }
            if (stack_target) {
                vma_remove(&space->vma_head, &space->vma_tree, stack_target);
                stack_target->start = page_vaddr;
                vma_insert(&space->vma_head, &space->vma_tree, stack_target);
                vma = stack_target;
            }
            vmm_up_write(&space->mmap_sem);
            vmm_down_read(&space->mmap_sem);

            if (!vma) {
                vmm_up_read(&space->mmap_sem);
                return -EFAULT;
            }
        } else {
            vmm_up_read(&space->mmap_sem);
            return -EFAULT;
        }
    }

    if ((error_code & 2) && !(vma->flags & VMA_FLAG_WRITE) && !(vma->flags & VMA_FLAG_COW)) {
        vmm_up_read(&space->mmap_sem);
        return -EACCES;
    }

    if ((error_code & 16) && !(vma->flags & VMA_FLAG_EXEC)) {
        vmm_up_read(&space->mmap_sem);
        return -EACCES;
    }

    if (!(error_code & 1)) {
        if (!(error_code & 2)) {
            if (zero_page_paddr != 0) {
                mmu_map_page(space->mmu_ctx, page_vaddr, zero_page_paddr, MMU_PROT_READ | MMU_PROT_USER);
                vmm_up_read(&space->mmap_sem);
                return 0;
            }
        }

        page_t *p = pmm_alloc_page(PAGE_FLAG_ZERO);
        if (!p) {
            vmm_up_read(&space->mmap_sem);
            return -ENOMEM;
        }

        uint32_t map_flags = MMU_PROT_READ | MMU_PROT_USER;
        if (vma->flags & VMA_FLAG_WRITE) map_flags |= MMU_PROT_WRITE;
        if (vma->flags & VMA_FLAG_EXEC)  map_flags |= MMU_PROT_EXEC;

        mmu_map_page(space->mmu_ctx, page_vaddr, pmm_page_to_paddr(p), map_flags);
        vmm_up_read(&space->mmap_sem);
        return 0;
    }

    if ((error_code & 1) && (error_code & 2)) {
        uint64_t *pte_ptr = mmu_get_pte_ptr(space->mmu_ctx, page_vaddr);
        if (!pte_ptr) {
            vmm_up_read(&space->mmap_sem);
            return -EFAULT;
        }

        uint64_t old_pte = *pte_ptr;
        uintptr_t old_paddr = old_pte & PT_ADDR_MASK;
        page_t *old_page = pmm_paddr_to_page(old_paddr);

        if (old_page && old_page != zero_page_desc && __atomic_load_n(&old_page->refcount, __ATOMIC_SEQ_CST) == 1) {
            __atomic_fetch_or(pte_ptr, PT_RW, __ATOMIC_SEQ_CST);
            mmu_tlb_flush_page(page_vaddr);
            mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), page_vaddr, 1);
            vmm_up_read(&space->mmap_sem);
            return 0;
        }

        page_t *new_page = pmm_alloc_page(0);
        if (!new_page) {
            vmm_up_read(&space->mmap_sem);
            return -ENOMEM;
        }

        uintptr_t new_paddr = pmm_page_to_paddr(new_page);
        memcpy((void *)p2v(new_paddr), (void *)p2v(old_paddr), PAGE_SIZE);

        #define PTE_IGNORE_HW_BITS (PT_ACCESSED | PT_DIRTY)
        uint64_t cur_pte = *pte_ptr;
        while (true) {
            if ((cur_pte & PT_PRESENT) && (cur_pte & PT_RW)) {
                pmm_free_page(new_page);
                mmu_tlb_flush_page(page_vaddr);
                mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), page_vaddr, 1);
                vmm_up_read(&space->mmap_sem);
                return 0;
            }

            if ((cur_pte & ~PTE_IGNORE_HW_BITS) != (old_pte & ~PTE_IGNORE_HW_BITS)) {
                pmm_free_page(new_page);
                vmm_up_read(&space->mmap_sem);
                return -EFAULT;
            }

            uint64_t desired_pte = (new_paddr & PT_ADDR_MASK) | (cur_pte & PTE_IGNORE_HW_BITS) | PT_PRESENT | PT_USER | PT_RW;
            if (vma->flags & VMA_FLAG_EXEC) desired_pte &= ~PT_NX;
            else desired_pte |= PT_NX;

            if (__atomic_compare_exchange_n(pte_ptr, &cur_pte, desired_pte, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                break;
            }
        }

        if (old_page && old_page != zero_page_desc) {
            pmm_free_page(old_page);
        }

        mmu_tlb_flush_page(page_vaddr);
        mmu_tlb_shootdown_target(__atomic_load_n(&space->active_cpus, __ATOMIC_SEQ_CST), page_vaddr, 1);
        vmm_up_read(&space->mmap_sem);
        return 0;
    }

    vmm_up_read(&space->mmap_sem);
    return -EACCES;
}

void *vmalloc(size_t size) {
    if (size == 0) return NULL;

    size_t aligned_size = (size + PAGE_MASK) & ~PAGE_MASK;
    size_t page_count = aligned_size >> 12;

    uint64_t flags = spinlock_acquire_irqsave(&vmalloc_lock);

    uintptr_t virt = vmalloc_current;
    if (virt + aligned_size > VMALLOC_END) {
        spinlock_release_irqrestore(&vmalloc_lock, flags);
        return NULL;
    }
    vmalloc_current += aligned_size;

    spinlock_release_irqrestore(&vmalloc_lock, flags);

    mmu_context_t *kctx = mmu_get_kernel_context();
    for (size_t i = 0; i < page_count; i++) {
        page_t *p = pmm_alloc_page(PAGE_FLAG_KERNEL | PAGE_FLAG_ZERO);
        if (!p) {
            for (size_t j = 0; j < i; j++) {
                uintptr_t v = virt + (j << 12);
                uintptr_t phys = mmu_virt_to_phys(kctx, v);
                if (phys) pmm_free_page(pmm_paddr_to_page(phys));
                mmu_unmap_page(kctx, v);
            }
            return NULL;
        }

        p->flags |= PAGE_FLAG_VMALLOC;
        p->index = (i == 0) ? page_count : 0;
        mmu_map_page(kctx, virt + (i << 12), pmm_page_to_paddr(p), MMU_PROT_READ | MMU_PROT_WRITE | MMU_FLAG_GLOBAL);
    }

    return (void *)virt;
}

void vfree(void *addr) {
    if (!addr) return;
    uintptr_t virt = (uintptr_t)addr;
    if (virt < VMALLOC_BASE || virt >= VMALLOC_END || (virt & PAGE_MASK)) return;

    mmu_context_t *kctx = mmu_get_kernel_context();
    uintptr_t first_phys = mmu_virt_to_phys(kctx, virt);
    if (!first_phys) return;

    page_t *p0 = pmm_paddr_to_page(first_phys);
    if (!p0 || !(p0->flags & PAGE_FLAG_VMALLOC)) return;

    size_t page_count = p0->index;
    if (page_count == 0) page_count = 1;

    for (size_t i = 0; i < page_count; i++) {
        uintptr_t curr = virt + (i << 12);
        uintptr_t phys = mmu_virt_to_phys(kctx, curr);
        if (phys) {
            page_t *p = pmm_paddr_to_page(phys);
            if (p) {
                p->flags &= ~PAGE_FLAG_VMALLOC;
                p->index = 0;
                pmm_free_page(p);
            }
            mmu_unmap_page(kctx, curr);
        }
    }

    mmu_tlb_flush_all();
}
