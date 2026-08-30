// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "vmm_test.h"
#include "../vmm.h"
#include "../pmm.h"
#include "../mmu.h"
#include "../../sys/process.h"
#include <string.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("[VMM TEST FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return false; \
        } \
    } while (0)

bool vmm_run_tests(void) {
    vmm_space_t *space = vmm_create_space();
    TEST_ASSERT(space != NULL, "vmm_create_space returned NULL");

    uintptr_t vaddr = (uintptr_t)vmm_map(space, 0, 0x4000, MMU_PROT_READ | MMU_PROT_WRITE, 0, NULL, 0);
    TEST_ASSERT(vaddr >= space->mmap_base, "vmm_map did not return address in mmap region");

    registers_t dummy_regs;
    memset(&dummy_regs, 0, sizeof(registers_t));
    dummy_regs.rsp = 0x00007FFFFFF00000ULL;

    int res = vmm_handle_page_fault(space, vaddr, 0, &dummy_regs);
    TEST_ASSERT(res == 0, "Read fault on unbacked VMA failed");

    uint64_t *pte = mmu_get_pte_ptr(space->mmu_ctx, vaddr);
    TEST_ASSERT(pte != NULL, "PTE was not populated after read fault");
    TEST_ASSERT((*pte & (1ULL << 0)), "PTE is not present");
    TEST_ASSERT(!(*pte & (1ULL << 1)), "Zero page was mapped writable on read fault");

    res = vmm_handle_page_fault(space, vaddr, 3, &dummy_regs);
    TEST_ASSERT(res == 0, "Write fault on zero page failed to allocate private frame");

    pte = mmu_get_pte_ptr(space->mmu_ctx, vaddr);
    TEST_ASSERT(pte != NULL, "PTE null after write fault");
    TEST_ASSERT((*pte & (1ULL << 1)), "PTE not writable after write fault");

    vm_area_t *stack_vma = vma_create(0x00007FFFFFE00000ULL, 0x00007FFFFFF00000ULL, VMA_TYPE_ANON,
                                      VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_STACK);
    TEST_ASSERT(stack_vma != NULL, "Failed to create stack VMA");
    vma_insert(&space->vma_head, &space->vma_tree, stack_vma);

    dummy_regs.rsp = 0x00007FFFFFE00000ULL;
    res = vmm_handle_page_fault(space, dummy_regs.rsp - 0x10000, 2, &dummy_regs);
    TEST_ASSERT(res != 0, "Stack-clash guard failed to reject out-of-bounds fault");

    res = vmm_handle_page_fault(space, dummy_regs.rsp - 64, 2, &dummy_regs);
    TEST_ASSERT(res == 0, "Red-zone stack extension failed");
    TEST_ASSERT(stack_vma->start <= dummy_regs.rsp - 64, "Stack VMA start was not extended downward");

    res = vmm_unmap(space, vaddr, 0x4000);
    TEST_ASSERT(res == 0, "vmm_unmap returned error");
    pte = mmu_get_pte_ptr(space->mmu_ctx, vaddr);
    TEST_ASSERT(pte == NULL || (*pte & 1) == 0, "PTE still present after vmm_unmap");

    vmm_destroy_space(space);

    size_t vmalloc_sz = 2 * 1024 * 1024;
    uint8_t *vbuf = (uint8_t *)vmalloc(vmalloc_sz);
    TEST_ASSERT(vbuf != NULL, "vmalloc(2MB) returned NULL");

    for (size_t i = 0; i < vmalloc_sz; i += 4096) {
        vbuf[i] = (uint8_t)(i & 0xFF);
    }
    for (size_t i = 0; i < vmalloc_sz; i += 4096) {
        TEST_ASSERT(vbuf[i] == (uint8_t)(i & 0xFF), "Data mismatch in vmalloc buffer");
    }
    vfree(vbuf);

    serial_write("[VMM TEST] All virtual memory manager unit tests passed successfully!\n");
    return true;
}
