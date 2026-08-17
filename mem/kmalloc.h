// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
/*
################################################################

BEWARE

THIS IS SCAFFOLDING, CAN VERY MUCH BE CHANGED!!

#################################################################

*/

#ifndef BOREDOS_KMALLOC_H
#define BOREDOS_KMALLOC_H

#include "slab.h"

#define KMALLOC_MAGIC 0x4B4D414CUL

typedef struct kmalloc_header {
    uint32_t magic;
    uint32_t size;
    slab_cache_t *cache;
} kmalloc_header_t;

void kmalloc_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void *krealloc(void *ptr, size_t new_size);
void kfree(void *ptr);

#define kfree_null(ptr) do { \
    __auto_type _p = &(ptr); \
    kfree(*_p); \
    *_p = NULL; \
} while (0)

#endif // BOREDOS_KMALLOC_H
