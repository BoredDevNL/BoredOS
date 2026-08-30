// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_RADIX_TREE_H
#define BOREDOS_RADIX_TREE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RADIX_TREE_MAP_SHIFT   6
#define RADIX_TREE_MAP_SIZE    (1UL << RADIX_TREE_MAP_SHIFT)
#define RADIX_TREE_MAP_MASK    (RADIX_TREE_MAP_SIZE - 1)

#define RADIX_TREE_TAG_DIRTY   0
#define RADIX_TREE_TAG_LOCKED  1
#define RADIX_TREE_MAX_TAGS    2

typedef struct radix_tree_node {
    uint8_t shift;
    uint8_t count;
    void *slots[RADIX_TREE_MAP_SIZE];
    uint64_t tags[RADIX_TREE_MAX_TAGS];
} radix_tree_node_t;

typedef struct radix_tree_root {
    uint8_t height;
    void *rnode;
} radix_tree_root_t;

#define RADIX_TREE_INIT { .height = 0, .rnode = NULL }

void radix_tree_init(radix_tree_root_t *root);
int radix_tree_insert(radix_tree_root_t *root, uint64_t index, void *item);
void *radix_tree_lookup(const radix_tree_root_t *root, uint64_t index);
void *radix_tree_delete(radix_tree_root_t *root, uint64_t index);

void radix_tree_tag_set(radix_tree_root_t *root, uint64_t index, unsigned int tag);
void radix_tree_tag_clear(radix_tree_root_t *root, uint64_t index, unsigned int tag);
int radix_tree_tag_get(const radix_tree_root_t *root, uint64_t index, unsigned int tag);

size_t radix_tree_gang_lookup(const radix_tree_root_t *root, void **results, uint64_t first_index, size_t max_items);
size_t radix_tree_gang_lookup_tag(const radix_tree_root_t *root, void **results, uint64_t first_index, size_t max_items, unsigned int tag);

#endif // BOREDOS_RADIX_TREE_H
