// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "mmu.h"
#include "pmm.h"
#include "platform.h"
#include "slab.h"
#include "msrs.h"
#include <string.h>

#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17

#define PAGE_SIZE 4096UL
#define PAGE_MASK (PAGE_SIZE - 1)
#define HUGE_PAGE_2M (2UL * 1024 * 1024)
#define HUGE_PAGE_2M_MASK (HUGE_PAGE_2M - 1)

#define PT_PRESENT       (1ULL << 0)
#define PT_RW            (1ULL << 1)
#define PT_USER          (1ULL << 2)
#define PT_PWT           (1ULL << 3)
#define PT_PCD           (1ULL << 4)
#define PT_ACCESSED      (1ULL << 5)
#define PT_DIRTY         (1ULL << 6)
#define PT_HUGE          (1ULL << 7)
#define PT_GLOBAL        (1ULL << 8)
#define PT_COW           (1ULL << 9)
#define PT_PAT_4K        (1ULL << 12)
#define PT_PAT_2M        (1ULL << 12)
#define PT_NX            (1ULL << 63)

#define PT_ADDR_MASK     0x000FFFFFFFFFF000ULL
#define PT_2M_ADDR_MASK  0x000FFFFFFFE00000ULL
#define PT_1G_ADDR_MASK  0x000FFFFFC0000000ULL

typedef struct {
    uint64_t entries[512];
} page_table_t;

static mmu_context_t kernel_context;
static mmu_context_t *active_context = &kernel_context;

static inline size_t pml4_idx(uintptr_t v) { return (v >> 39) & 0x1FF; }
static inline size_t pdpt_idx(uintptr_t v) { return (v >> 30) & 0x1FF; }
static inline size_t pd_idx(uintptr_t v)   { return (v >> 21) & 0x1FF; }
static inline size_t pt_idx(uintptr_t v)   { return (v >> 12) & 0x1FF; }

static inline page_table_t *p2table(uint64_t entry) {
    return (page_table_t *)p2v(entry & PT_ADDR_MASK);
}

static inline uint64_t read_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void write_cr3(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

static inline void invlpg(uintptr_t virt) {
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static void pat_init(void) {
    uint64_t pat = rdmsr(0x277);
    pat &= ~(0xFFULL << 32);
    pat |=  (0x01ULL << 32);
    wrmsr(0x277, pat);
}

static inline uint64_t mmu_flags_to_pte(uint32_t flags) {
    uint64_t pte = PT_PRESENT;

    if ((flags & MMU_PROT_WRITE) && !(flags & MMU_FLAG_COW)) pte |= PT_RW;
    if (flags & MMU_PROT_USER) pte |= PT_USER;
    if (!(flags & MMU_PROT_EXEC)) pte |= PT_NX;
    if (flags & MMU_FLAG_GLOBAL) pte |= PT_GLOBAL;
    if (flags & MMU_FLAG_COW) pte |= PT_COW;

    if (flags & MMU_FLAG_WC) {
        pte |= PT_PAT_4K;
    } else if (flags & MMU_FLAG_NOCACHE) {
        pte |= PT_PCD | PT_PWT;
    }
    return pte;
}

static page_table_t *alloc_table_frame(uintptr_t *out_phys) {
    page_t *p = pmm_alloc_page(PAGE_FLAG_KERNEL | PAGE_FLAG_ZERO);
    if (p) {
        uintptr_t phys = pmm_page_to_paddr(p);
        page_table_t *table = (page_table_t *)p2v(phys);
        memset(table, 0, PAGE_SIZE);
        if (out_phys) *out_phys = phys;
        return table;
    }

    extern void *kmalloc_aligned(size_t size, size_t alignment);
    void *ptr = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!ptr) return NULL;
    memset(ptr, 0, PAGE_SIZE);
    if (out_phys) *out_phys = v2p((uintptr_t)ptr);
    return (page_table_t *)ptr;
}

static void free_table_frame(uintptr_t phys) {
    page_t *p = pmm_paddr_to_page(phys);
    if (p) {
        pmm_free_page(p);
    } else {
        kfree((void *)p2v(phys));
    }
}

static inline bool is_table_empty(const page_table_t *table) {
    for (int i = 0; i < 512; i++) {
        if (table->entries[i] != 0) return false;
    }
    return true;
}

void mmu_init(void) {
    pat_init();
    kernel_context.pml4_phys = read_cr3() & PT_ADDR_MASK;
    kernel_context.lock = SPINLOCK_INIT;
    active_context = &kernel_context;
}

mmu_context_t *mmu_get_kernel_context(void) {
    return &kernel_context;
}

mmu_context_t *mmu_get_current_context(void) {
    return active_context ? active_context : &kernel_context;
}

mmu_context_t *mmu_create_context(void) {
    mmu_context_t *ctx = (mmu_context_t *)kmalloc(sizeof(mmu_context_t));
    if (!ctx) return NULL;

    uintptr_t pml4_phys = 0;
    page_table_t *user_pml4 = alloc_table_frame(&pml4_phys);
    if (!user_pml4) {
        kfree(ctx);
        return NULL;
    }

    page_table_t *k_pml4 = (page_table_t *)p2v(kernel_context.pml4_phys);
    for (int i = 256; i < 512; i++) {
        user_pml4->entries[i] = k_pml4->entries[i];
    }

    ctx->pml4_phys = pml4_phys;
    ctx->lock = SPINLOCK_INIT;
    return ctx;
}

static void destroy_pd(uintptr_t pd_phys) {
    page_table_t *pd = (page_table_t *)p2v(pd_phys);
    for (int i = 0; i < 512; i++) {
        uint64_t pde = pd->entries[i];
        if ((pde & PT_PRESENT) && !(pde & PT_HUGE)) {
            free_table_frame(pde & PT_ADDR_MASK);
        }
    }
    free_table_frame(pd_phys);
}

static void destroy_pdpt(uintptr_t pdpt_phys) {
    page_table_t *pdpt = (page_table_t *)p2v(pdpt_phys);
    for (int i = 0; i < 512; i++) {
        uint64_t pdpte = pdpt->entries[i];
        if ((pdpte & PT_PRESENT) && !(pdpte & PT_HUGE)) {
            destroy_pd(pdpte & PT_ADDR_MASK);
        }
    }
    free_table_frame(pdpt_phys);
}

void mmu_destroy_context(mmu_context_t *ctx) {
    if (!ctx || ctx == &kernel_context || !ctx->pml4_phys) return;

    if (active_context == ctx) {
        mmu_switch_context(&kernel_context);
    }

    page_table_t *pml4 = (page_table_t *)p2v(ctx->pml4_phys);
    for (int i = 0; i < 256; i++) {
        uint64_t pml4e = pml4->entries[i];
        if (pml4e & PT_PRESENT) {
            destroy_pdpt(pml4e & PT_ADDR_MASK);
        }
    }

    free_table_frame(ctx->pml4_phys);
    kfree(ctx);
}

void mmu_switch_context(mmu_context_t *ctx) {
    if (!ctx || !ctx->pml4_phys) return;

    active_context = ctx;
    uint64_t current_cr3 = read_cr3();
    if ((current_cr3 & PT_ADDR_MASK) != ctx->pml4_phys) {
        write_cr3(ctx->pml4_phys);
    }
}

void mmu_tlb_flush_page(uintptr_t virt) {
    invlpg(virt);
}

void mmu_tlb_flush_all(void) {
    write_cr3(read_cr3());
}

void mmu_tlb_shootdown(uintptr_t virt, size_t count) {
    for (size_t i = 0; i < count; i++) {
        invlpg(virt + i * PAGE_SIZE);
    }
}

static int mmu_map_page_unlocked(mmu_context_t *ctx, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    page_table_t *pml4 = (page_table_t *)p2v(ctx->pml4_phys);
    size_t l4 = pml4_idx(virt);
    size_t l3 = pdpt_idx(virt);
    size_t l2 = pd_idx(virt);
    size_t l1 = pt_idx(virt);

    uintptr_t new_pdpt_phys = 0;
    uintptr_t new_pd_phys   = 0;
    uintptr_t new_pt_phys   = 0;

    page_table_t *pdpt = NULL;
    if (!(pml4->entries[l4] & PT_PRESENT)) {
        pdpt = alloc_table_frame(&new_pdpt_phys);
        if (!pdpt) return -ENOMEM;
        pml4->entries[l4] = new_pdpt_phys | PT_PRESENT | PT_RW | PT_USER;
    } else {
        pdpt = p2table(pml4->entries[l4]);
    }

    if (pdpt->entries[l3] & PT_HUGE) {
        if (new_pdpt_phys) {
            pml4->entries[l4] = 0;
            free_table_frame(new_pdpt_phys);
        }
        return -EEXIST;
    }

    page_table_t *pd = NULL;
    if (!(pdpt->entries[l3] & PT_PRESENT)) {
        pd = alloc_table_frame(&new_pd_phys);
        if (!pd) {
            if (new_pdpt_phys) {
                pml4->entries[l4] = 0;
                free_table_frame(new_pdpt_phys);
            }
            return -ENOMEM;
        }
        pdpt->entries[l3] = new_pd_phys | PT_PRESENT | PT_RW | PT_USER;
    } else {
        pd = p2table(pdpt->entries[l3]);
    }

    if (pd->entries[l2] & PT_HUGE) {
        if (new_pd_phys) {
            pdpt->entries[l3] = 0;
            free_table_frame(new_pd_phys);
        }
        if (new_pdpt_phys) {
            pml4->entries[l4] = 0;
            free_table_frame(new_pdpt_phys);
        }
        return -EEXIST;
    }

    if (flags & MMU_FLAG_HUGE_2M) {
        if ((virt & HUGE_PAGE_2M_MASK) || (phys & HUGE_PAGE_2M_MASK)) {
            return -EINVAL;
        }

        uint64_t pde_val = (phys & PT_2M_ADDR_MASK) | mmu_flags_to_pte(flags) | PT_HUGE;
        if (flags & MMU_FLAG_WC) pde_val |= PT_PAT_2M;

        uint64_t old_pde = __atomic_exchange_n(&pd->entries[l2], pde_val, __ATOMIC_SEQ_CST);
        if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
            invlpg(virt);
        }

        (void)old_pde;
        return 0;
    }

    page_table_t *pt = NULL;
    if (!(pd->entries[l2] & PT_PRESENT)) {
        pt = alloc_table_frame(&new_pt_phys);
        if (!pt) {
            if (new_pd_phys) {
                pdpt->entries[l3] = 0;
                free_table_frame(new_pd_phys);
            }
            if (new_pdpt_phys) {
                pml4->entries[l4] = 0;
                free_table_frame(new_pdpt_phys);
            }
            return -ENOMEM;
        }
        pd->entries[l2] = new_pt_phys | PT_PRESENT | PT_RW | PT_USER;
    } else {
        pt = p2table(pd->entries[l2]);
    }

    uint64_t pte_val = (phys & PT_ADDR_MASK) | mmu_flags_to_pte(flags);
    uint64_t old_pte = __atomic_exchange_n(&pt->entries[l1], pte_val, __ATOMIC_SEQ_CST);

    if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
        invlpg(virt);
    }

    (void)old_pte;
    return 0;
}

int mmu_map_page(mmu_context_t *ctx, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    if (!ctx || !ctx->pml4_phys) return -EINVAL;
    if ((virt & PAGE_MASK) || (phys & PAGE_MASK)) return -EINVAL;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);
    int ret = mmu_map_page_unlocked(ctx, virt, phys, flags);
    spinlock_release_irqrestore(&ctx->lock, rflags);
    return ret;
}

static int mmu_unmap_page_unlocked(mmu_context_t *ctx, uintptr_t virt) {
    page_table_t *pml4 = (page_table_t *)p2v(ctx->pml4_phys);
    size_t l4 = pml4_idx(virt);
    size_t l3 = pdpt_idx(virt);
    size_t l2 = pd_idx(virt);
    size_t l1 = pt_idx(virt);

    if (!(pml4->entries[l4] & PT_PRESENT)) return 0;

    page_table_t *pdpt = p2table(pml4->entries[l4]);
    if (!(pdpt->entries[l3] & PT_PRESENT)) return 0;
    if (pdpt->entries[l3] & PT_HUGE) return -EINVAL;

    page_table_t *pd = p2table(pdpt->entries[l3]);
    if (!(pd->entries[l2] & PT_PRESENT)) return 0;

    if (pd->entries[l2] & PT_HUGE) {
        uint64_t old_pde = __atomic_exchange_n(&pd->entries[l2], 0ULL, __ATOMIC_SEQ_CST);
        if (old_pde & PT_PRESENT) {
            uintptr_t phys = old_pde & PT_2M_ADDR_MASK;
            page_t *pg = pmm_paddr_to_page(phys);
            if (pg && (old_pde & PT_DIRTY)) {
                pg->flags |= PAGE_FLAG_DIRTY;
            }
            if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
                invlpg(virt);
            }
        }
        if (l4 < 256 && is_table_empty(pd)) {
            uintptr_t pd_phys = pdpt->entries[l3] & PT_ADDR_MASK;
            pdpt->entries[l3] = 0;
            free_table_frame(pd_phys);
            if (is_table_empty(pdpt)) {
                uintptr_t pdpt_phys = pml4->entries[l4] & PT_ADDR_MASK;
                pml4->entries[l4] = 0;
                free_table_frame(pdpt_phys);
            }
        }
        return 0;
    }

    page_table_t *pt = p2table(pd->entries[l2]);
    uint64_t old_pte = __atomic_exchange_n(&pt->entries[l1], 0ULL, __ATOMIC_SEQ_CST);

    if (old_pte & PT_PRESENT) {
        uintptr_t phys = old_pte & PT_ADDR_MASK;
        page_t *pg = pmm_paddr_to_page(phys);
        if (pg && (old_pte & PT_DIRTY)) {
            pg->flags |= PAGE_FLAG_DIRTY;
        }

        if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
            invlpg(virt);
        }
    }

    if (l4 < 256 && is_table_empty(pt)) {
        uintptr_t pt_phys = pd->entries[l2] & PT_ADDR_MASK;
        pd->entries[l2] = 0;
        free_table_frame(pt_phys);

        if (is_table_empty(pd)) {
            uintptr_t pd_phys = pdpt->entries[l3] & PT_ADDR_MASK;
            pdpt->entries[l3] = 0;
            free_table_frame(pd_phys);

            if (is_table_empty(pdpt)) {
                uintptr_t pdpt_phys = pml4->entries[l4] & PT_ADDR_MASK;
                pml4->entries[l4] = 0;
                free_table_frame(pdpt_phys);
            }
        }
    }

    return 0;
}

int mmu_unmap_page(mmu_context_t *ctx, uintptr_t virt) {
    if (!ctx || !ctx->pml4_phys || (virt & PAGE_MASK)) return -EINVAL;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);
    int ret = mmu_unmap_page_unlocked(ctx, virt);
    spinlock_release_irqrestore(&ctx->lock, rflags);
    return ret;
}

int mmu_map_pages(mmu_context_t *ctx, uintptr_t virt, uintptr_t phys, size_t count, uint32_t flags) {
    if (!ctx || !ctx->pml4_phys) return -EINVAL;
    if ((virt & PAGE_MASK) || (phys & PAGE_MASK)) return -EINVAL;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);
    for (size_t i = 0; i < count; i++) {
        int err = mmu_map_page_unlocked(ctx, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
        if (err != 0) {
            for (size_t j = 0; j < i; j++) {
                mmu_unmap_page_unlocked(ctx, virt + j * PAGE_SIZE);
            }
            spinlock_release_irqrestore(&ctx->lock, rflags);
            return err;
        }
    }
    spinlock_release_irqrestore(&ctx->lock, rflags);
    return 0;
}

int mmu_unmap_pages(mmu_context_t *ctx, uintptr_t virt, size_t count) {
    if (!ctx || !ctx->pml4_phys || (virt & PAGE_MASK)) return -EINVAL;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);
    for (size_t i = 0; i < count; i++) {
        mmu_unmap_page_unlocked(ctx, virt + i * PAGE_SIZE);
    }
    spinlock_release_irqrestore(&ctx->lock, rflags);
    return 0;
}

int mmu_protect_page(mmu_context_t *ctx, uintptr_t virt, uint32_t flags) {
    if (!ctx || !ctx->pml4_phys) return -EINVAL;
    if (virt & PAGE_MASK) return -EINVAL;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);

    page_table_t *pml4 = (page_table_t *)p2v(ctx->pml4_phys);
    size_t l4 = pml4_idx(virt);
    size_t l3 = pdpt_idx(virt);
    size_t l2 = pd_idx(virt);
    size_t l1 = pt_idx(virt);

    if (!(pml4->entries[l4] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return -EINVAL;
    }

    page_table_t *pdpt = p2table(pml4->entries[l4]);
    if (!(pdpt->entries[l3] & PT_PRESENT) || (pdpt->entries[l3] & PT_HUGE)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return -EINVAL;
    }

    page_table_t *pd = p2table(pdpt->entries[l3]);
    if (!(pd->entries[l2] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return -EINVAL;
    }

    if (pd->entries[l2] & PT_HUGE) {
        uint64_t phys = pd->entries[l2] & PT_2M_ADDR_MASK;
        uint64_t new_pde = phys | mmu_flags_to_pte(flags) | PT_HUGE;
        uint64_t old_pde = __atomic_exchange_n(&pd->entries[l2], new_pde, __ATOMIC_SEQ_CST);

        if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
            invlpg(virt);
        }

        (void)old_pde;
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return 0;
    }

    page_table_t *pt = p2table(pd->entries[l2]);
    if (!(pt->entries[l1] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return -EINVAL;
    }

    uint64_t phys = pt->entries[l1] & PT_ADDR_MASK;
    uint64_t new_pte = phys | mmu_flags_to_pte(flags);
    uint64_t old_pte = __atomic_exchange_n(&pt->entries[l1], new_pte, __ATOMIC_SEQ_CST);

    if (old_pte & PT_DIRTY) {
        page_t *pg = pmm_paddr_to_page(phys);
        if (pg) pg->flags |= PAGE_FLAG_DIRTY;
    }

    if ((read_cr3() & PT_ADDR_MASK) == ctx->pml4_phys) {
        invlpg(virt);
    }

    spinlock_release_irqrestore(&ctx->lock, rflags);
    return 0;
}

uintptr_t mmu_virt_to_phys(mmu_context_t *ctx, uintptr_t virt) {
    if (!ctx || !ctx->pml4_phys) return 0;

    uint64_t rflags = spinlock_acquire_irqsave(&ctx->lock);

    page_table_t *pml4 = (page_table_t *)p2v(ctx->pml4_phys);
    size_t l4 = pml4_idx(virt);
    size_t l3 = pdpt_idx(virt);
    size_t l2 = pd_idx(virt);
    size_t l1 = pt_idx(virt);

    if (!(pml4->entries[l4] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return 0;
    }

    page_table_t *pdpt = p2table(pml4->entries[l4]);
    if (!(pdpt->entries[l3] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return 0;
    }

    if (pdpt->entries[l3] & PT_HUGE) {
        uintptr_t phys = (pdpt->entries[l3] & PT_1G_ADDR_MASK) | (virt & 0x3FFFFFFFULL);
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return phys;
    }

    page_table_t *pd = p2table(pdpt->entries[l3]);
    if (!(pd->entries[l2] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return 0;
    }

    if (pd->entries[l2] & PT_HUGE) {
        uintptr_t phys = (pd->entries[l2] & PT_2M_ADDR_MASK) | (virt & 0x1FFFFFULL);
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return phys;
    }

    page_table_t *pt = p2table(pd->entries[l2]);
    if (!(pt->entries[l1] & PT_PRESENT)) {
        spinlock_release_irqrestore(&ctx->lock, rflags);
        return 0;
    }

    uintptr_t phys = (pt->entries[l1] & PT_ADDR_MASK) | (virt & PAGE_MASK);
    spinlock_release_irqrestore(&ctx->lock, rflags);
    return phys;
}
