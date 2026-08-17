// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "slab.h"
#include <stddef.h>

void slab_init(void) {
}

slab_cache_t *slab_cache_create(const char *name, size_t obj_size, size_t align, uint32_t flags) {
    (void)name; (void)obj_size; (void)align; (void)flags;
    return NULL;
}

void slab_cache_destroy(slab_cache_t *cache) {
    (void)cache;
}

void *slab_cache_alloc(slab_cache_t *cache) {
    (void)cache;
    return NULL;
}

void slab_cache_free(slab_cache_t *cache, void *obj) {
    (void)cache; (void)obj;
}
