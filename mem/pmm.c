// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pmm.h"
#include <stddef.h>

static uintptr_t pmm_direct_map_base = 0;

void pmm_set_direct_map(uintptr_t direct_map_base) {
    pmm_direct_map_base = direct_map_base;
}

uintptr_t pmm_get_direct_map(void) {
    return pmm_direct_map_base;
}

void pmm_init(const pmm_boot_map_t *boot_map) {
    if (boot_map) {
        pmm_direct_map_base = boot_map->direct_map_base;
    }
}

page_t *pmm_alloc_page(uint32_t flags) {
    (void)flags;
    return NULL;
}

page_t *pmm_alloc_pages(size_t count, uint32_t flags) {
    (void)count; (void)flags;
    return NULL;
}

void pmm_free_page(page_t *page) {
    (void)page;
}

void pmm_free_pages(page_t *page, size_t count) {
    (void)page; (void)count;
}

void pmm_page_ref(page_t *page) {
    if (page) page->refcount++;
}

void pmm_page_unref(page_t *page) {
    if (page && page->refcount > 0) page->refcount--;
}

page_t *pmm_paddr_to_page(uintptr_t paddr) {
    (void)paddr;
    return NULL;
}

uintptr_t pmm_page_to_paddr(const page_t *page) {
    (void)page;
    return 0;
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
    return stats;
}
