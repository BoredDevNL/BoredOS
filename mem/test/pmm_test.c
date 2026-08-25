// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pmm_test.h"
#include "pmm.h"
#include "platform.h"
#include <string.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("[PMM TEST FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return false; \
        } \
    } while (0)

bool pmm_run_tests(void) {
    page_t *p0 = pmm_alloc_page(0);
    TEST_ASSERT(p0 != NULL, "Order-0 alloc returned NULL");
    TEST_ASSERT(__atomic_load_n(&p0->refcount, __ATOMIC_SEQ_CST) == 1, "Order-0 initial refcount must be 1");
    TEST_ASSERT(p0->order == 0, "Order-0 order mismatch");

    uintptr_t p0_phys = pmm_page_to_paddr(p0);
    TEST_ASSERT((p0_phys & 0xFFF) == 0, "Allocated paddr must be 4 KiB aligned");
    TEST_ASSERT(pmm_paddr_to_page(p0_phys) == p0, "paddr_to_page roundtrip failed");

    pmm_free_page(p0);

    page_t *pz = pmm_alloc_page(PAGE_FLAG_ZERO);
    TEST_ASSERT(pz != NULL, "PAGE_FLAG_ZERO alloc returned NULL");

    uint8_t *pz_vaddr = (uint8_t *)p2v(pmm_page_to_paddr(pz));
    bool is_zero = true;
    for (size_t i = 0; i < PMM_PAGE_SIZE; i++) {
        if (pz_vaddr[i] != 0) {
            is_zero = false;
            break;
        }
    }
    TEST_ASSERT(is_zero, "PAGE_FLAG_ZERO memory contains non-zero bytes");
    pmm_free_page(pz);

    page_t *p_huge = pmm_alloc_order(9, 0);
    TEST_ASSERT(p_huge != NULL, "Order 9 (2 MiB) huge page alloc failed");
    uintptr_t huge_phys = pmm_page_to_paddr(p_huge);
    TEST_ASSERT((huge_phys & 0x1FFFFFULL) == 0, "Order 9 block must be 2 MiB aligned");
    TEST_ASSERT(p_huge->order == 9, "Order 9 descriptor order mismatch");
    pmm_free_order(p_huge, 9);

    page_t *p_dma = pmm_alloc_page(PAGE_FLAG_DMA);
    TEST_ASSERT(p_dma != NULL, "PAGE_FLAG_DMA alloc returned NULL");
    uintptr_t dma_phys = pmm_page_to_paddr(p_dma);
    TEST_ASSERT(dma_phys < 0x01000000ULL, "PAGE_FLAG_DMA must allocate < 16 MiB");
    pmm_free_page(p_dma);

    page_t *p_dma32 = pmm_alloc_page(PAGE_FLAG_DMA32);
    TEST_ASSERT(p_dma32 != NULL, "PAGE_FLAG_DMA32 alloc returned NULL");
    uintptr_t dma32_phys = pmm_page_to_paddr(p_dma32);
    TEST_ASSERT(dma32_phys < 0x100000000ULL, "PAGE_FLAG_DMA32 must allocate < 4 GiB");
    pmm_free_page(p_dma32);

    page_t *pref = pmm_alloc_page(0);
    TEST_ASSERT(pref != NULL, "Alloc for refcount test failed");
    TEST_ASSERT(__atomic_load_n(&pref->refcount, __ATOMIC_SEQ_CST) == 1, "Initial refcount must be 1");

    pmm_page_ref(pref);
    TEST_ASSERT(__atomic_load_n(&pref->refcount, __ATOMIC_SEQ_CST) == 2, "Refcount must be 2 after pmm_page_ref");

    pmm_page_unref(pref);
    TEST_ASSERT(__atomic_load_n(&pref->refcount, __ATOMIC_SEQ_CST) == 1, "Refcount must be 1 after first pmm_page_unref");
    TEST_ASSERT(!(pref->flags & PAGE_FLAG_FREE), "Page must still be active when refcount == 1");

    pmm_page_unref(pref);
    TEST_ASSERT(pref->flags & PAGE_FLAG_FREE, "Page must be marked free when refcount hits 0");

    page_t *p_count3 = pmm_alloc_pages(3, 0);
    TEST_ASSERT(p_count3 != NULL, "pmm_alloc_pages(3) returned NULL");
    TEST_ASSERT(p_count3->order == 2, "pmm_alloc_pages(3) should round up to Order 2");
    pmm_free_pages(p_count3, 3);
    TEST_ASSERT(p_count3->flags & PAGE_FLAG_FREE, "pmm_free_pages(3) should free block");

    page_t *p_count7 = pmm_alloc_pages(7, 0);
    TEST_ASSERT(p_count7 != NULL, "pmm_alloc_pages(7) returned NULL");
    TEST_ASSERT(p_count7->order == 3, "pmm_alloc_pages(7) should round up to Order 3");
    pmm_free_pages(p_count7, 7);

    page_t *p_parent = pmm_alloc_order(1, 0);
    TEST_ASSERT(p_parent != NULL, "Failed to allocate order-1 parent block");
    TEST_ASSERT(p_parent->order == 1, "Allocated block must have order 1");
    uintptr_t base_pfn = pmm_page_to_paddr(p_parent) >> 12;
    TEST_ASSERT((base_pfn & 1) == 0, "Order-1 block base PFN must be 2-page aligned");

    page_t *buddy_page = pmm_paddr_to_page((base_pfn + 1) << 12);
    TEST_ASSERT(buddy_page != NULL, "Order-1 buddy descriptor must exist");
    uintptr_t pfn1 = base_pfn;
    uintptr_t pfn2 = pmm_page_to_paddr(buddy_page) >> 12;
    TEST_ASSERT((pfn1 ^ 1) == pfn2, "Split pages must be adjacent buddies");

    pmm_free_order(p_parent, 1);

    page_t *ptest = pmm_alloc_page(0);
    TEST_ASSERT(ptest != NULL, "Alloc for address helper test failed");
    void *vaddr = pmm_page_to_vaddr(ptest);
    TEST_ASSERT(vaddr != NULL, "pmm_page_to_vaddr returned NULL");
    TEST_ASSERT(pmm_vaddr_to_page(vaddr) == ptest, "pmm_vaddr_to_page roundtrip failed");
    pmm_free_page(ptest);

    pmm_init_percpu();
    page_t *pcp_pages[48];
    for (size_t i = 0; i < 48; i++) {
        pcp_pages[i] = pmm_alloc_page(0);
        TEST_ASSERT(pcp_pages[i] != NULL, "PCP batch allocation failed");
    }
    for (size_t i = 0; i < 48; i++) {
        pmm_free_page(pcp_pages[i]);
    }

    TEST_ASSERT(pmm_alloc_order(PMM_MAX_ORDER, 0) == NULL, "Alloc order >= MAX_ORDER must return NULL");
    TEST_ASSERT(pmm_alloc_order(PMM_MAX_ORDER + 5, 0) == NULL, "Alloc order >> MAX_ORDER must return NULL");
    TEST_ASSERT(pmm_alloc_pages(0, 0) == NULL, "Alloc 0 pages must return NULL");

    pmm_stats_t s1 = pmm_get_stats();
    TEST_ASSERT(s1.total_pages > 0, "total_pages must be > 0");
    TEST_ASSERT(s1.total_pages == (s1.free_pages + s1.reserved_pages), "total_pages must equal free + reserved");

    page_t *p_stat = pmm_alloc_page(0);
    TEST_ASSERT(p_stat != NULL, "Alloc for stats test failed");
    pmm_free_page(p_stat);

    serial_write("[PMM TEST] All physical memory manager unit tests passed successfully!\n");
    return true;
}
