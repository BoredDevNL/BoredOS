// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "slab_test.h"
#include "slab.h"
#include <string.h>

extern void serial_write(const char *str);

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            serial_write("[SLAB TEST FAIL] "); \
            serial_write(msg); \
            serial_write("\n"); \
            return false; \
        } \
    } while (0)

typedef struct {
    uint32_t magic;
    uint32_t data;
} slab_test_item_t;

#define TEST_MAGIC 0xCAFEFACE

static void test_item_ctor(void *obj, size_t size) {
    (void)size;
    slab_test_item_t *item = (slab_test_item_t *)obj;
    item->magic = TEST_MAGIC;
    item->data = 0;
}

bool slab_run_tests(void) {
    slab_cache_t *test_cache = slab_cache_create_with_ctor("test_ctor_cache", sizeof(slab_test_item_t),
                                                           16, SLAB_FLAG_NONE, test_item_ctor, NULL);
    TEST_ASSERT(test_cache != NULL, "Failed to create custom slab cache");

    slab_test_item_t *item1 = (slab_test_item_t *)slab_cache_alloc(test_cache);
    TEST_ASSERT(item1 != NULL, "slab_cache_alloc returned NULL");
    TEST_ASSERT(item1->magic == TEST_MAGIC, "Constructor was not executed on allocated object");

    item1->data = 999;
    slab_cache_free(test_cache, item1);

    slab_test_item_t *item2 = (slab_test_item_t *)slab_cache_alloc(test_cache);
    TEST_ASSERT(item2 != NULL, "Second slab_cache_alloc returned NULL");
    TEST_ASSERT(item2->magic == TEST_MAGIC, "Constructor magic corrupted while in cache");
    TEST_ASSERT(item2->data == 999, "Constructed object state was not preserved across free/alloc");
    slab_cache_free(test_cache, item2);

    void *p16 = kmalloc(16);
    TEST_ASSERT(p16 != NULL, "kmalloc(16) returned NULL");
    memset(p16, 0xAA, 16);
    kfree(p16);

    void *p256 = kmalloc(256);
    TEST_ASSERT(p256 != NULL, "kmalloc(256) returned NULL");
    memset(p256, 0xBB, 256);
    kfree(p256);

    void *p1024 = kmalloc(1024);
    TEST_ASSERT(p1024 != NULL, "kmalloc(1024) returned NULL");
    memset(p1024, 0xCC, 1024);
    kfree(p1024);

    void *p2048 = kmalloc(2048);
    TEST_ASSERT(p2048 != NULL, "kmalloc(2048) returned NULL");
    memset(p2048, 0xDD, 2048);
    kfree(p2048);

    uint8_t *pz = (uint8_t *)kzalloc(128);
    TEST_ASSERT(pz != NULL, "kzalloc(128) returned NULL");
    bool all_zero = true;
    for (size_t i = 0; i < 128; i++) {
        if (pz[i] != 0) {
            all_zero = false;
            break;
        }
    }
    TEST_ASSERT(all_zero, "kzalloc memory contains non-zero bytes");
    kfree(pz);

    void *p4k = kmalloc(4096);
    TEST_ASSERT(p4k != NULL, "kmalloc(4096) returned NULL");
    memset(p4k, 0x11, 4096);
    kfree(p4k);

    void *p16k = kmalloc(16384);
    TEST_ASSERT(p16k != NULL, "kmalloc(16384) returned NULL");
    memset(p16k, 0x22, 16384);
    kfree(p16k);

    void *p64k = kmalloc(65536);
    TEST_ASSERT(p64k != NULL, "kmalloc(65536) returned NULL");
    memset(p64k, 0x33, 65536);
    kfree(p64k);

    char *str = (char *)kmalloc(32);
    TEST_ASSERT(str != NULL, "kmalloc for krealloc failed");
    strcpy(str, "BoredOS SLAB");

    char *str_expanded = (char *)krealloc(str, 128);
    TEST_ASSERT(str_expanded != NULL, "krealloc(128) returned NULL");
    TEST_ASSERT(strcmp(str_expanded, "BoredOS SLAB") == 0, "krealloc did not preserve original contents");
    kfree(str_expanded);

    void *objs[64];
    for (size_t i = 0; i < 64; i++) {
        objs[i] = slab_cache_alloc(test_cache);
        TEST_ASSERT(objs[i] != NULL, "Magazine batch alloc failed");
    }
    for (size_t i = 0; i < 64; i++) {
        slab_cache_free(test_cache, objs[i]);
    }
    for (size_t i = 0; i < 64; i++) {
        objs[i] = slab_cache_alloc(test_cache);
        TEST_ASSERT(objs[i] != NULL, "Magazine batch re-alloc failed");
    }
    for (size_t i = 0; i < 64; i++) {
        slab_cache_free(test_cache, objs[i]);
    }

    slab_cache_reap(test_cache);
    slab_cache_destroy(test_cache);

    serial_write("[SLAB TEST] All slab allocator unit tests passed successfully!\n");
    return true;
}
