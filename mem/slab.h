// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
/*
################################################################

BEWARE

THIS IS SCAFFOLDING, CAN VERY MUCH BE CHANGED!!

#################################################################

*/

#ifndef BOREDOS_SLAB_H
#define BOREDOS_SLAB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SLAB_FLAG_NONE       0x00
#define SLAB_FLAG_POISON     0x01
#define SLAB_FLAG_ZERO       0x02
#define SLAB_FLAG_PER_CPU    0x04

typedef struct slab_cache slab_cache_t;

void slab_init(void);
slab_cache_t *slab_cache_create(const char *name, size_t obj_size, size_t align, uint32_t flags);
void slab_cache_destroy(slab_cache_t *cache);
void *slab_cache_alloc(slab_cache_t *cache);
void slab_cache_free(slab_cache_t *cache, void *obj);

#endif // BOREDOS_SLAB_H
