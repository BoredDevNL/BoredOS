// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "limine.h"

// SLAB_CLASSES and SLAB_MAX_SIZE must stay in sync with the slab_sizes[] table in memory_manager.c.
#define DEFAULT_POOL_SIZE           (128 * 1024 * 1024)
#define BLOCK_LIST_INITIAL_CAPACITY 1024
#define SLAB_CLASSES  7
#define SLAB_MAX_SIZE 512

typedef struct {
    void *address;
    size_t size;
    bool allocated;
    uint32_t allocation_id;  // Monotonically increasing; wraps at UINT32_MAX. 0 = unallocated.
    uint32_t timestamp;      // Value of the internal tick counter at allocation time; 0 = unallocated.
} MemBlock;

// Snapshot of allocator state returned by memory_get_stats().
// fragmentation_percent is the proportion of free memory stranded outside the largest free block.
typedef struct {
    size_t total_memory;
    size_t used_memory;
    size_t available_memory;
    size_t allocated_blocks;
    size_t free_blocks;
    size_t largest_free_block;
    size_t smallest_free_block;
    size_t fragmentation_percent;
    size_t peak_memory_used;
    size_t slab_allocs;
    size_t slab_frees;
} MemStats;

// Must be called exactly once before any allocation. Idempotent if called again (no-op),
// but re-initialisation after allocations have been made is not supported.
void memory_manager_init_from_memmap(struct limine_memmap_response *memmap);

void* kmalloc(size_t size);
void* kcalloc(size_t n, size_t size);
// alignment must be a power of 2. Requests <= 512 B with alignment <= 8 are served by the slab allocator.
void* kmalloc_aligned(size_t size, size_t alignment);
void kfree(void *ptr);
void* krealloc(void *ptr, size_t new_size);

MemStats memory_get_stats(void);
bool mm_is_heap_address(void *ptr);

#endif // MEMORY_MANAGER_H
