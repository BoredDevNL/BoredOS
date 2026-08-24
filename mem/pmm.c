// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pmm.h"
#include "spinlock.h"
#include "platform.h"
#include "smp.h"
#include "kutils.h"
#include <string.h>

#define PMM_DMA_LIMIT   0x01000000ULL       // 16 MiB
#define PMM_DMA32_LIMIT 0x100000000ULL      // 4 GiB

#define PCP_BATCH_SIZE 16
#define PCP_MAX_PAGES  32
#define PMM_MAX_CPUS   64

typedef struct {
    spinlock_t lock;
    uintptr_t base_paddr;
    uintptr_t end_paddr;
    page_t *free_lists[PMM_MAX_ORDER];
    size_t free_pages;
} pmm_zone_t;

typedef struct {
    uint32_t count;
    page_t *pages[PCP_MAX_PAGES];
} pcp_pool_t;

static pmm_zone_t zones[PMM_ZONE_COUNT];
static pcp_pool_t pcp_pools[PMM_MAX_CPUS];
static bool pmm_smp_ready = false;

static page_t *pages_array = NULL;
static size_t total_page_count = 0;
static size_t usable_page_count = 0;
static uintptr_t pmm_direct_map_base = 0;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static inline uintptr_t page_to_pfn(const page_t *page) {
    return (uintptr_t)(page - pages_array);
}

static inline page_t *pfn_to_page(uintptr_t pfn) {
    if (pfn >= total_page_count || !pages_array) return NULL;
    return &pages_array[pfn];
}

static inline uint8_t pages_to_order(size_t count) {
    if (count <= 1) return 0;
    if (count > (1UL << (PMM_MAX_ORDER - 1))) return PMM_MAX_ORDER;
    return (uint8_t)(64 - __builtin_clzll(count - 1));
}

static inline void list_add_head(page_t **head, page_t *page) {
    page->free_prev = NULL;
    page->free_next = *head;
    if (*head) (*head)->free_prev = page;
    *head = page;
}

static inline void list_remove(page_t **head, page_t *page) {
    if (page->free_prev) {
        page->free_prev->free_next = page->free_next;
    } else if (*head == page) {
        *head = page->free_next;
    }
    if (page->free_next) {
        page->free_next->free_prev = page->free_prev;
    }
    page->free_prev = NULL;
    page->free_next = NULL;
}

static inline uint8_t paddr_to_zone_id(uintptr_t paddr) {
    if (paddr < PMM_DMA_LIMIT) return PMM_ZONE_DMA;
    if (paddr < PMM_DMA32_LIMIT) return PMM_ZONE_DMA32;
    return PMM_ZONE_NORMAL;
}

void pmm_set_direct_map(uintptr_t direct_map_base) {
    pmm_direct_map_base = direct_map_base;
}

uintptr_t pmm_get_direct_map(void) {
    return pmm_direct_map_base;
}

void pmm_init_percpu(void) {
    for (size_t c = 0; c < PMM_MAX_CPUS; c++) {
        pcp_pools[c].count = 0;
    }
    pmm_smp_ready = true;
}

static void add_free_block(uintptr_t base_pfn, uint8_t order) {
    page_t *page = pfn_to_page(base_pfn);
    if (!page) return;

    uint8_t zone_id = page->zone;
    pmm_zone_t *z = &zones[zone_id];

    page->order = order;
    page->flags = PAGE_FLAG_FREE;
    page->refcount = 0;

    list_add_head(&z->free_lists[order], page);
    z->free_pages += (1UL << order);
    usable_page_count += (1UL << order);
}

static void ingest_usable_range(uintptr_t start_paddr, uintptr_t end_paddr) {
    uintptr_t curr_pfn = start_paddr >> PMM_PAGE_SHIFT;
    uintptr_t end_pfn = end_paddr >> PMM_PAGE_SHIFT;

    while (curr_pfn < end_pfn) {
        uint8_t order = 0;
        while (order < (PMM_MAX_ORDER - 1)) {
            uintptr_t block_size = 1UL << (order + 1);
            if ((curr_pfn & (block_size - 1)) != 0 || (curr_pfn + block_size) > end_pfn) {
                break;
            }
            uint8_t z1 = paddr_to_zone_id(curr_pfn << PMM_PAGE_SHIFT);
            uint8_t z2 = paddr_to_zone_id(((curr_pfn + block_size - 1) << PMM_PAGE_SHIFT));
            if (z1 != z2) break;
            order++;
        }

        add_free_block(curr_pfn, order);
        curr_pfn += (1UL << order);
    }
}

void pmm_init(const pmm_boot_map_t *boot_map) {
    if (!boot_map || boot_map->region_count == 0) return;
    pmm_direct_map_base = boot_map->direct_map_base;
    usable_page_count = 0;

    for (size_t z = 0; z < PMM_ZONE_COUNT; z++) {
        zones[z].lock = SPINLOCK_INIT;
        zones[z].free_pages = 0;
        for (size_t o = 0; o < PMM_MAX_ORDER; o++) {
            zones[z].free_lists[o] = NULL;
        }
    }

    zones[PMM_ZONE_DMA].base_paddr = 0;
    zones[PMM_ZONE_DMA].end_paddr = PMM_DMA_LIMIT;

    zones[PMM_ZONE_DMA32].base_paddr = PMM_DMA_LIMIT;
    zones[PMM_ZONE_DMA32].end_paddr = PMM_DMA32_LIMIT;

    zones[PMM_ZONE_NORMAL].base_paddr = PMM_DMA32_LIMIT;
    zones[PMM_ZONE_NORMAL].end_paddr = (uintptr_t)-1;

    uintptr_t max_paddr = 0;
    for (size_t i = 0; i < boot_map->region_count; i++) {
        const pmm_mem_region_t *r = &boot_map->regions[i];
        if (r->type != PMM_REGION_USABLE) continue;
        uintptr_t top = r->base + r->length;
        if (top > max_paddr) max_paddr = top;
    }

    total_page_count = max_paddr >> PMM_PAGE_SHIFT;
    size_t array_size = (total_page_count * sizeof(page_t) + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);

    uintptr_t array_paddr = 0;
    size_t max_usable_len = 0;
    for (size_t i = 0; i < boot_map->region_count; i++) {
        const pmm_mem_region_t *r = &boot_map->regions[i];
        if (r->type == PMM_REGION_USABLE && r->length > max_usable_len) {
            max_usable_len = r->length;
            array_paddr = (r->base + 0xFFFULL) & ~0xFFFULL;
        }
    }

    if (array_paddr == 0 || max_usable_len < array_size) return;

    pages_array = (page_t *)p2v(array_paddr);
    memset(pages_array, 0, array_size);

    for (size_t pfn = 0; pfn < total_page_count; pfn++) {
        pages_array[pfn].flags = PAGE_FLAG_RESERVED;
        pages_array[pfn].refcount = 1;
        pages_array[pfn].order = 0;
        pages_array[pfn].zone = paddr_to_zone_id(pfn << PMM_PAGE_SHIFT);
    }

    uintptr_t array_pfn_start = array_paddr >> PMM_PAGE_SHIFT;
    uintptr_t array_pfn_end = (array_paddr + array_size) >> PMM_PAGE_SHIFT;

    for (size_t i = 0; i < boot_map->region_count; i++) {
        const pmm_mem_region_t *r = &boot_map->regions[i];
        if (r->type != PMM_REGION_USABLE) continue;

        uintptr_t aligned_base = (r->base + 0xFFFULL) & ~0xFFFULL;
        uintptr_t aligned_end = (r->base + r->length) & ~0xFFFULL;
        if (aligned_end <= aligned_base) continue;

        uintptr_t reg_pfn_start = aligned_base >> PMM_PAGE_SHIFT;
        uintptr_t reg_pfn_end = aligned_end >> PMM_PAGE_SHIFT;

        if (reg_pfn_end <= array_pfn_start || reg_pfn_start >= array_pfn_end) {
            ingest_usable_range(aligned_base, aligned_end);
        } else {
            if (reg_pfn_start < array_pfn_start) {
                ingest_usable_range(reg_pfn_start << PMM_PAGE_SHIFT, array_pfn_start << PMM_PAGE_SHIFT);
            }
            if (reg_pfn_end > array_pfn_end) {
                ingest_usable_range(array_pfn_end << PMM_PAGE_SHIFT, reg_pfn_end << PMM_PAGE_SHIFT);
            }
        }
    }
}

static page_t *alloc_from_zone_locked(pmm_zone_t *z, uint8_t order, uint32_t flags) {
    for (uint8_t curr_order = order; curr_order < PMM_MAX_ORDER; curr_order++) {
        if (!z->free_lists[curr_order]) continue;

        page_t *page = z->free_lists[curr_order];
        list_remove(&z->free_lists[curr_order], page);

        while (curr_order > order) {
            curr_order--;
            uintptr_t base_pfn = page_to_pfn(page);
            uintptr_t buddy_pfn = base_pfn + (1UL << curr_order);
            page_t *buddy = pfn_to_page(buddy_pfn);

            buddy->order = curr_order;
            buddy->zone = page->zone;
            buddy->flags = PAGE_FLAG_FREE;
            buddy->refcount = 0;
            list_add_head(&z->free_lists[curr_order], buddy);
        }

        page->order = order;
        page->flags = (flags & ~PAGE_FLAG_ZERO);
        page->refcount = 1;
        z->free_pages -= (1UL << order);
        return page;
    }
    return NULL;
}

page_t *pmm_alloc_order(uint8_t order, uint32_t flags) {
    if (order >= PMM_MAX_ORDER) return NULL;

    uint32_t cpu_id = smp_this_cpu_id();
    if (cpu_id >= PMM_MAX_CPUS) cpu_id = 0;

    if (order == 0 && (flags == 0 || flags == PAGE_FLAG_ZERO) && pmm_smp_ready) {
        pcp_pool_t *pcp = &pcp_pools[cpu_id];
        uint64_t rflags = irq_save();

        if (pcp->count > 0) {
            pcp->count--;
            page_t *page = pcp->pages[pcp->count];
            irq_restore(rflags);

            page->order = 0;
            page->flags = (flags & ~PAGE_FLAG_ZERO);
            page->refcount = 1;

            if (flags & PAGE_FLAG_ZERO) {
                page_zero_fast((void *)p2v(pmm_page_to_paddr(page)));
            }
            return page;
        }

        pmm_zone_t *z = &zones[PMM_ZONE_NORMAL];
        uint64_t zflags = spinlock_acquire_irqsave(&z->lock);
        if (z->free_pages == 0) {
            spinlock_release_irqrestore(&z->lock, zflags);
            z = &zones[PMM_ZONE_DMA32];
            zflags = spinlock_acquire_irqsave(&z->lock);
        }

        page_t *batch[PCP_BATCH_SIZE];
        size_t fetched = 0;
        for (size_t i = 0; i < PCP_BATCH_SIZE; i++) {
            page_t *p = alloc_from_zone_locked(z, 0, 0);
            if (!p) break;
            batch[fetched++] = p;
        }
        spinlock_release_irqrestore(&z->lock, zflags);

        if (fetched > 0) {
            for (size_t i = 1; i < fetched; i++) {
                pcp->pages[pcp->count++] = batch[i];
            }
            irq_restore(rflags);

            page_t *ret = batch[0];
            ret->order = 0;
            ret->flags = (flags & ~PAGE_FLAG_ZERO);
            ret->refcount = 1;

            if (flags & PAGE_FLAG_ZERO) {
                page_zero_fast((void *)p2v(pmm_page_to_paddr(ret)));
            }
            return ret;
        }
        irq_restore(rflags);
    }

    int start_zone = PMM_ZONE_NORMAL;
    int min_zone = PMM_ZONE_DMA;

    if (flags & PAGE_FLAG_DMA) {
        start_zone = PMM_ZONE_DMA;
        min_zone = PMM_ZONE_DMA;
    } else if (flags & PAGE_FLAG_DMA32) {
        start_zone = PMM_ZONE_DMA32;
        min_zone = PMM_ZONE_DMA;
    }

    page_t *allocated = NULL;
    for (int z_idx = start_zone; z_idx >= min_zone; z_idx--) {
        pmm_zone_t *z = &zones[z_idx];
        uint64_t rflags = spinlock_acquire_irqsave(&z->lock);
        allocated = alloc_from_zone_locked(z, order, flags);
        spinlock_release_irqrestore(&z->lock, rflags);
        if (allocated) break;
    }

    if (allocated && (flags & PAGE_FLAG_ZERO)) {
        uintptr_t phys = pmm_page_to_paddr(allocated);
        size_t pages = (1UL << order);
        for (size_t i = 0; i < pages; i++) {
            page_zero_fast((void *)p2v(phys + i * PMM_PAGE_SIZE));
        }
    }

    return allocated;
}

page_t *pmm_alloc_page(uint32_t flags) {
    return pmm_alloc_order(0, flags);
}

page_t *pmm_alloc_pages(size_t count, uint32_t flags) {
    if (count == 0) return NULL;
    uint8_t order = pages_to_order(count);
    if (order >= PMM_MAX_ORDER) return NULL;
    return pmm_alloc_order(order, flags);
}

static void free_to_zone_locked(pmm_zone_t *z, page_t *page, uint8_t order) {
    uintptr_t pfn = page_to_pfn(page);
    uint8_t curr_order = order;

    while (curr_order < (PMM_MAX_ORDER - 1)) {
        uintptr_t buddy_pfn = pfn ^ (1UL << curr_order);
        if (buddy_pfn >= total_page_count) break;

        page_t *buddy = pfn_to_page(buddy_pfn);
        if (!buddy) break;
        if (!(buddy->flags & PAGE_FLAG_FREE)) break;
        if (buddy->order != curr_order) break;
        if (buddy->zone != page->zone) break;
        if (buddy->refcount != 0) break;

        list_remove(&z->free_lists[curr_order], buddy);
        if (buddy_pfn < pfn) {
            page = buddy;
            pfn = buddy_pfn;
        }
        curr_order++;
    }

    page->order = curr_order;
    page->flags = PAGE_FLAG_FREE;
    page->refcount = 0;
    list_add_head(&z->free_lists[curr_order], page);
    z->free_pages += (1UL << order);
}

void pmm_free_order(page_t *page, uint8_t order) {
    if (!page || order >= PMM_MAX_ORDER) return;

    uint32_t prev = __atomic_fetch_sub(&page->refcount, 1, __ATOMIC_SEQ_CST);
    if (prev > 1) {
        return;
    }

    uint32_t cpu_id = smp_this_cpu_id();
    if (cpu_id >= PMM_MAX_CPUS) cpu_id = 0;

    if (order == 0 && page->zone == PMM_ZONE_NORMAL && pmm_smp_ready) {
        pcp_pool_t *pcp = &pcp_pools[cpu_id];
        uint64_t rflags = irq_save();

        if (pcp->count < PCP_MAX_PAGES) {
            pcp->pages[pcp->count++] = page;
            irq_restore(rflags);
            return;
        }

        page_t *drain_batch[PCP_BATCH_SIZE];
        for (size_t i = 0; i < PCP_BATCH_SIZE; i++) {
            pcp->count--;
            drain_batch[i] = pcp->pages[pcp->count];
        }
        pcp->pages[pcp->count++] = page;
        irq_restore(rflags);

        pmm_zone_t *z = &zones[PMM_ZONE_NORMAL];
        uint64_t zflags = spinlock_acquire_irqsave(&z->lock);
        for (size_t i = 0; i < PCP_BATCH_SIZE; i++) {
            free_to_zone_locked(z, drain_batch[i], 0);
        }
        spinlock_release_irqrestore(&z->lock, zflags);
        return;
    }

    pmm_zone_t *z = &zones[page->zone];
    uint64_t rflags = spinlock_acquire_irqsave(&z->lock);
    free_to_zone_locked(z, page, order);
    spinlock_release_irqrestore(&z->lock, rflags);
}

void pmm_free_page(page_t *page) {
    pmm_free_order(page, 0);
}

void pmm_free_pages(page_t *page, size_t count) {
    if (!page || count == 0) return;
    uint8_t order = pages_to_order(count);
    if (order >= PMM_MAX_ORDER) return;
    pmm_free_order(page, order);
}

void pmm_page_ref(page_t *page) {
    if (page) {
        __atomic_fetch_add(&page->refcount, 1, __ATOMIC_SEQ_CST);
    }
}

void pmm_page_unref(page_t *page) {
    if (!page) return;
    pmm_free_order(page, page->order);
}

page_t *pmm_paddr_to_page(uintptr_t paddr) {
    return pfn_to_page(paddr >> PMM_PAGE_SHIFT);
}

uintptr_t pmm_page_to_paddr(const page_t *page) {
    if (!page || !pages_array) return 0;
    return page_to_pfn(page) << PMM_PAGE_SHIFT;
}

page_t *pmm_vaddr_to_page(const void *vaddr) {
    if (!vaddr || pmm_direct_map_base == 0) return NULL;
    uintptr_t paddr = (uintptr_t)vaddr - pmm_direct_map_base;
    return pmm_paddr_to_page(paddr);
}

void *pmm_page_to_vaddr(const page_t *page) {
    if (!page || pmm_direct_map_base == 0) return NULL;
    uintptr_t paddr = pmm_page_to_paddr(page);
    return (void *)(paddr + pmm_direct_map_base);
}

pmm_stats_t pmm_get_stats(void) {
    pmm_stats_t stats = {0};
    stats.total_pages = usable_page_count;
    for (size_t z = 0; z < PMM_ZONE_COUNT; z++) {
        stats.free_pages += zones[z].free_pages;
    }
    stats.reserved_pages = (stats.total_pages > stats.free_pages) ? (stats.total_pages - stats.free_pages) : 0;
    return stats;
}
