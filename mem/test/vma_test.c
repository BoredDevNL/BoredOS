// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.


/* 
* ##############################################
*
* Should be removed before pr to main is opened!
*
* ##############################################
*/
#include "vma_test.h"
#include "vma.h"
#include <stddef.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("[VMA TEST FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return false; \
        } \
    } while (0)

bool vma_run_tests(void) {
    vm_area_t *head = NULL;
    rb_root_t root = RB_ROOT;

    TEST_ASSERT(vma_create(0x2000, 0x1000, VMA_TYPE_ANON, 0) == NULL, "Invalid bounds must fail");

    vm_area_t *v1 = vma_create(0x1000, 0x3000, VMA_TYPE_ANON, VMA_FLAG_READ | VMA_FLAG_WRITE);
    vm_area_t *v2 = vma_create(0x5000, 0x8000, VMA_TYPE_ANON, VMA_FLAG_READ);
    vm_area_t *v3 = vma_create(0x3000, 0x5000, VMA_TYPE_ANON, VMA_FLAG_READ | VMA_FLAG_EXEC);
    vm_area_t *v4 = vma_create(0xA000, 0xC000, VMA_TYPE_FILE, VMA_FLAG_READ);

    TEST_ASSERT(v1 && v2 && v3 && v4, "Failed to create test VMAs");

    TEST_ASSERT(vma_insert(&head, &root, v1), "Failed to insert v1");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v1 insert");

    TEST_ASSERT(vma_insert(&head, &root, v2), "Failed to insert v2");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v2 insert");

    TEST_ASSERT(vma_insert(&head, &root, v3), "Failed to insert v3");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v3 insert");

    TEST_ASSERT(vma_insert(&head, &root, v4), "Failed to insert v4");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v4 insert");

    vm_area_t *overlap = vma_create(0x2000, 0x4000, VMA_TYPE_ANON, 0);
    TEST_ASSERT(!vma_insert(&head, &root, overlap), "Overlapping VMA insert should have failed");
    vma_destroy(overlap);

    TEST_ASSERT(vma_find(&root, 0x1500) == v1, "Find failed for address inside v1");
    TEST_ASSERT(vma_find(&root, 0x6000) == v2, "Find failed for address inside v2");
    TEST_ASSERT(vma_find(&root, 0x4500) == v3, "Find failed for address inside v3");
    TEST_ASSERT(vma_find(&root, 0xB000) == v4, "Find failed for address inside v4");
    TEST_ASSERT(vma_find(&root, 0x9000) == NULL, "Find in unmapped gap should return NULL");

    uintptr_t gap = vma_find_unmapped_area(&root, 0x1000, 0x10000, 0x1000, 0x1000);
    TEST_ASSERT(gap == 0x8000, "Gap find failed to identify [0x8000, 0xA000) hole");

    vm_area_t *left = NULL;
    vm_area_t *right = NULL;
    TEST_ASSERT(vma_split(v2, 0x6000, &left, &right), "Failed to split v2");
    TEST_ASSERT(left->start == 0x5000 && left->end == 0x6000, "Left split bounds invalid");
    TEST_ASSERT(right->start == 0x6000 && right->end == 0x8000, "Right split bounds invalid");
    TEST_ASSERT(vma_insert(&head, &root, right), "Failed to re-insert right split VMA");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after split insertion");

    TEST_ASSERT(vma_merge_adjacent(&head, &root, left, right) != NULL, "Failed to merge split adjacent VMAs");
    TEST_ASSERT(left->start == 0x5000 && left->end == 0x8000, "Merged VMA bounds invalid");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after merge");

    TEST_ASSERT(vma_remove(&head, &root, v1), "Failed to remove v1");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v1 removal");
    vma_destroy(v1);

    TEST_ASSERT(vma_remove(&head, &root, v3), "Failed to remove v3");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after v3 removal");
    vma_destroy(v3);

    TEST_ASSERT(vma_remove(&head, &root, left), "Failed to remove left (merged v2)");
    TEST_ASSERT(vma_verify_invariants(&root), "Invariants violated after left removal");
    vma_destroy(left);

    TEST_ASSERT(vma_remove(&head, &root, v4), "Failed to remove v4");
    TEST_ASSERT(root.rb_node == NULL && head == NULL, "Full teardown failed to clear root/head");
    vma_destroy(v4);

    serial_write("[VMA TEST] All unit tests passed successfully!\n");
    return true;
}
