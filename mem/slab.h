// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_SLAB_H
#define BOREDOS_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SLAB_FLAG_NONE       0x00
#define SLAB_FLAG_POISON     0x01
#define SLAB_FLAG_ZERO       0x02
#define SLAB_FLAG_PER_CPU    0x04
#define SLAB_FLAG_RCU        0x08

#define SLAB_MIN_ALIGN       16
#define SLAB_SIMD_ALIGN      16
#define SLAB_AVX_ALIGN       32

typedef void (*slab_ctor_t)(void *obj, size_t size);
typedef void (*slab_dtor_t)(void *obj, size_t size);

typedef struct slab_cache slab_cache_t;

void slab_init(void);
slab_cache_t *slab_cache_create(const char *name, size_t obj_size, size_t align, uint32_t flags);
slab_cache_t *slab_cache_create_with_ctor(const char *name, size_t obj_size, size_t align, uint32_t flags,
                                          slab_ctor_t ctor, slab_dtor_t dtor);
void slab_cache_destroy(slab_cache_t *cache);
void *slab_cache_alloc(slab_cache_t *cache);
void slab_cache_free(slab_cache_t *cache, void *obj);
size_t slab_cache_reap(slab_cache_t *cache);

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
bool mm_is_heap_address(void *ptr);

#define kfree_null(p) do { if (p) { kfree(p); (p) = NULL; } } while (0)

#endif // BOREDOS_SLAB_H
