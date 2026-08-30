// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pagecache_test.h"
#include "../pagecache.h"
#include "../radix_tree.h"
#include "../pmm.h"
#include "platform.h"
#include "kutils.h"
#include <string.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        serial_write("[PAGECACHE TEST FAIL] "); \
        serial_write(msg); \
        serial_write("\n"); \
        return; \
    } \
} while(0)

static void test_radix_tree_basic(void) {
    radix_tree_root_t root;
    radix_tree_init(&root);

    int dummy1 = 100, dummy2 = 200, dummy3 = 300;
    TEST_ASSERT(radix_tree_insert(&root, 0, &dummy1) == 0, "Insert index 0 failed");
    TEST_ASSERT(radix_tree_insert(&root, 65, &dummy2) == 0, "Insert index 65 failed");
    TEST_ASSERT(radix_tree_insert(&root, 4096, &dummy3) == 0, "Insert index 4096 failed");

    TEST_ASSERT(radix_tree_lookup(&root, 0) == &dummy1, "Lookup index 0 failed");
    TEST_ASSERT(radix_tree_lookup(&root, 65) == &dummy2, "Lookup index 65 failed");
    TEST_ASSERT(radix_tree_lookup(&root, 4096) == &dummy3, "Lookup index 4096 failed");
    TEST_ASSERT(radix_tree_lookup(&root, 1) == NULL, "Lookup non-existent index failed");

    radix_tree_tag_set(&root, 65, RADIX_TREE_TAG_DIRTY);
    TEST_ASSERT(radix_tree_tag_get(&root, 65, RADIX_TREE_TAG_DIRTY) == 1, "Tag get dirty failed");
    TEST_ASSERT(radix_tree_tag_get(&root, 0, RADIX_TREE_TAG_DIRTY) == 0, "Tag get clean failed");

    void *results[10];
    size_t found = radix_tree_gang_lookup_tag(&root, results, 0, 10, RADIX_TREE_TAG_DIRTY);
    TEST_ASSERT(found == 1 && results[0] == &dummy2, "Gang lookup tag failed");

    TEST_ASSERT(radix_tree_delete(&root, 65) == &dummy2, "Delete index 65 failed");
    TEST_ASSERT(radix_tree_lookup(&root, 65) == NULL, "Lookup after delete failed");
    TEST_ASSERT(radix_tree_tag_get(&root, 65, RADIX_TREE_TAG_DIRTY) == 0, "Tag after delete failed");
}

static int mock_read_page(address_space_t *mapping, page_t *page) {
    (void)mapping;
    uintptr_t phys = pmm_page_to_paddr(page);
    memset((void *)p2v(phys), 0xAA, 4096);
    return 0;
}

static const address_space_ops_t mock_aops = {
    .read_page = mock_read_page,
    .write_page = NULL,
    .bmap = NULL,
};

static void test_address_space_caching(void) {
    address_space_t mapping;
    address_space_init(&mapping, NULL, &mock_aops);

    page_t *p0 = pagecache_get_page(&mapping, 0);
    TEST_ASSERT(p0 != NULL, "Pagecache get page 0 failed");
    uintptr_t phys0 = pmm_page_to_paddr(p0);
    uint8_t *data0 = (uint8_t *)p2v(phys0);
    TEST_ASSERT(data0[0] == 0xAA && data0[4095] == 0xAA, "Pagecache read page content failed");

    page_t *p0_cached = pagecache_find_page(&mapping, 0);
    TEST_ASSERT(p0_cached == p0, "Pagecache find page hit failed");
    pmm_free_page(p0_cached);

    page_t *p1 = pagecache_get_page(&mapping, 1);
    TEST_ASSERT(p1 != NULL && p1 != p0, "Pagecache get page 1 failed");

    pagecache_truncate_range(&mapping, 4096);
    TEST_ASSERT(pagecache_find_page(&mapping, 1) == NULL, "Truncated page 1 still found in cache");
    page_t *p0_check = pagecache_find_page(&mapping, 0);
    TEST_ASSERT(p0_check != NULL, "Page 0 wrongly truncated");
    if (p0_check) pmm_free_page(p0_check);

    address_space_destroy(&mapping);
}

bool pagecache_run_tests(void) {
    test_radix_tree_basic();
    test_address_space_caching();
    serial_write("[PAGECACHE TEST] All unit tests passed successfully!\n");
    return true;
}

void test_pagecache_all(void) {
    pagecache_run_tests();
}
