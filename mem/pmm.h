// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_PMM_H
#define BOREDOS_PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PMM_PAGE_SIZE 4096UL
#define PMM_PAGE_SHIFT 12
#define PMM_MAX_ORDER 18

#define PAGE_FLAG_FREE      (1 << 0)
#define PAGE_FLAG_RESERVED  (1 << 1)
#define PAGE_FLAG_KERNEL    (1 << 2)
#define PAGE_FLAG_USER      (1 << 3)
#define PAGE_FLAG_COW       (1 << 4)
#define PAGE_FLAG_ZERO      (1 << 5)
#define PAGE_FLAG_CACHED    (1 << 6)
#define PAGE_FLAG_DIRTY     (1 << 7)
#define PAGE_FLAG_LOCKED    (1 << 8)
#define PAGE_FLAG_DMA       (1 << 9)
#define PAGE_FLAG_DMA32     (1 << 10)
#define PAGE_FLAG_SLAB      (1 << 11)
#define PAGE_FLAG_KMALLOC_LARGE (1 << 12)
#define PAGE_FLAG_VMALLOC   (1 << 13)

typedef enum {
    PMM_ZONE_DMA = 0,
    PMM_ZONE_DMA32,
    PMM_ZONE_NORMAL,
    PMM_ZONE_COUNT
} pmm_zone_id_t;

typedef enum {
    PMM_REGION_USABLE = 1,
    PMM_REGION_RESERVED,
    PMM_REGION_ACPI_RECLAIM,
    PMM_REGION_ACPI_NVS,
    PMM_REGION_BAD_RAM,
    PMM_REGION_BOOTLOADER_RECLAIM,
    PMM_REGION_KERNEL,
    PMM_REGION_FRAMEBUFFER
} pmm_region_type_t;

typedef struct {
    uintptr_t base;
    size_t length;
    pmm_region_type_t type;
} pmm_mem_region_t;

typedef struct {
    pmm_mem_region_t *regions;
    size_t region_count;
    uintptr_t direct_map_base;
} pmm_boot_map_t;

typedef struct page {
    uint32_t flags;
    _Atomic uint32_t refcount;
    uint8_t order;
    uint8_t zone;

    union {
        struct {
            struct page *free_prev;
            struct page *free_next;
        };
        struct {
            void *mapping;
            uint64_t index;
            struct page *cache_next;
            struct page *lru_prev;
            struct page *lru_next;
        };
    };
} page_t;

typedef struct {
    size_t total_pages;
    size_t free_pages;
    size_t reserved_pages;
    size_t kernel_pages;
    size_t user_pages;
} pmm_stats_t;

void pmm_init(const pmm_boot_map_t *boot_map);
void pmm_init_percpu(void);

void pmm_set_direct_map(uintptr_t direct_map_base);
uintptr_t pmm_get_direct_map(void);

page_t *pmm_alloc_page(uint32_t flags);
page_t *pmm_alloc_order(uint8_t order, uint32_t flags);
page_t *pmm_alloc_pages(size_t count, uint32_t flags);

void pmm_free_page(page_t *page);
void pmm_free_order(page_t *page, uint8_t order);
void pmm_free_pages(page_t *page, size_t count);

void pmm_page_ref(page_t *page);
void pmm_page_unref(page_t *page);

page_t *pmm_paddr_to_page(uintptr_t paddr);
uintptr_t pmm_page_to_paddr(const page_t *page);
page_t *pmm_vaddr_to_page(const void *vaddr);
void *pmm_page_to_vaddr(const page_t *page);

pmm_stats_t pmm_get_stats(void);

static inline uint8_t pages_to_order(size_t count) {
    if (count <= 1) return 0;
    if (count > (1UL << (PMM_MAX_ORDER - 1))) return PMM_MAX_ORDER;
    return (uint8_t)(64 - __builtin_clzll(count - 1));
}

#endif // BOREDOS_PMM_H
