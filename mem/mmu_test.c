// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "mmu_test.h"
#include "mmu.h"
#include "pmm.h"
#include <stddef.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("[MMU TEST FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return false; \
        } \
    } while (0)

bool mmu_run_tests(void) {
    mmu_context_t *kctx = mmu_get_kernel_context();
    TEST_ASSERT(kctx != NULL && kctx->pml4_phys != 0, "Kernel context invalid");

    TEST_ASSERT(mmu_map_page(kctx, 0xFFFFA00000000001UL, 0x2000000UL, MMU_PROT_READ) != 0, "Unaligned virt map must fail");
    TEST_ASSERT(mmu_map_page(kctx, 0xFFFFA00000000000UL, 0x2000001UL, MMU_PROT_READ) != 0, "Unaligned phys map must fail");

    uintptr_t k_test_virt = 0xFFFFA00000000000UL;
    uintptr_t k_test_phys = 0x2000000UL;

    TEST_ASSERT(mmu_map_page(kctx, k_test_virt, k_test_phys, MMU_PROT_READ | MMU_PROT_WRITE) == 0, "Failed to map kernel test page");
    TEST_ASSERT(mmu_virt_to_phys(kctx, k_test_virt) == k_test_phys, "Kernel virt to phys mismatch");

    TEST_ASSERT(mmu_protect_page(kctx, k_test_virt, MMU_PROT_READ | MMU_FLAG_COW) == 0, "Failed to protect kernel page for CoW");
    TEST_ASSERT(mmu_virt_to_phys(kctx, k_test_virt) == k_test_phys, "Kernel virt to phys changed unexpectedly after protect");

    TEST_ASSERT(mmu_unmap_page(kctx, k_test_virt) == 0, "Failed to unmap kernel page");
    TEST_ASSERT(mmu_virt_to_phys(kctx, k_test_virt) == 0, "Unmapped kernel virt still translated");

    mmu_context_t *uctx = mmu_create_context();
    TEST_ASSERT(uctx != NULL && uctx->pml4_phys != 0, "Failed to create user context");
    TEST_ASSERT(uctx->pml4_phys != kctx->pml4_phys, "User context PML4 must be distinct from kernel");

    uintptr_t u_test_virt = 0x00007FFF00000000UL;
    uintptr_t u_test_phys = 0x3000000UL;

    TEST_ASSERT(mmu_map_page(uctx, u_test_virt, u_test_phys, MMU_PROT_READ | MMU_PROT_WRITE | MMU_PROT_USER) == 0, "Failed to map user page");
    TEST_ASSERT(mmu_virt_to_phys(uctx, u_test_virt) == u_test_phys, "User context translation failed");
    TEST_ASSERT(mmu_virt_to_phys(kctx, u_test_virt) == 0, "User mapping leaked into kernel context");

    TEST_ASSERT(mmu_unmap_page(uctx, u_test_virt) == 0, "Failed to unmap user page");
    TEST_ASSERT(mmu_virt_to_phys(uctx, u_test_virt) == 0, "Unmapped user virt still translated");

    uintptr_t batch_virt = 0x00007FFF10000000UL;
    uintptr_t batch_phys = 0x4000000UL;
    size_t page_count = 4;

    TEST_ASSERT(mmu_map_pages(uctx, batch_virt, batch_phys, page_count, MMU_PROT_READ | MMU_PROT_WRITE | MMU_PROT_USER) == 0, "Failed batch map");
    for (size_t i = 0; i < page_count; i++) {
        TEST_ASSERT(mmu_virt_to_phys(uctx, batch_virt + i * 4096) == batch_phys + i * 4096, "Batch page translation mismatch");
    }

    TEST_ASSERT(mmu_unmap_pages(uctx, batch_virt, page_count) == 0, "Failed batch unmap");
    for (size_t i = 0; i < page_count; i++) {
        TEST_ASSERT(mmu_virt_to_phys(uctx, batch_virt + i * 4096) == 0, "Unmapped batch page still present");
    }

    uintptr_t huge_virt = 0x00007FFF20000000UL;
    uintptr_t huge_phys = 0x6000000UL;

    TEST_ASSERT(mmu_map_page(uctx, huge_virt, huge_phys, MMU_PROT_READ | MMU_PROT_WRITE | MMU_FLAG_HUGE_2M | MMU_PROT_USER) == 0, "Failed 2MB huge page map");
    TEST_ASSERT(mmu_virt_to_phys(uctx, huge_virt + 0x12345UL) == huge_phys + 0x12345UL, "2MB huge page offset translation failed");

    TEST_ASSERT(mmu_map_page(uctx, huge_virt, 0x7000000UL, MMU_PROT_READ | MMU_PROT_USER) != 0, "4 KiB collision with 2 MiB page must fail");

    TEST_ASSERT(mmu_unmap_page(uctx, huge_virt) == 0, "Failed to unmap 2MB huge page");
    TEST_ASSERT(mmu_virt_to_phys(uctx, huge_virt) == 0, "Unmapped 2MB huge page still translated");

    mmu_destroy_context(uctx);

    serial_write("[MMU TEST] All hardware page table unit tests passed successfully!\n");
    return true;
}
