// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "radix_tree.h"
#include "slab.h"
#include <string.h>

static uint64_t height_to_maxindex[8];

static void init_maxindex(void) {
    static bool inited = false;
    if (inited) return;
    uint64_t max = 0;
    for (int i = 0; i < 8; i++) {
        if (i == 0) max = 0;
        else max = (max << RADIX_TREE_MAP_SHIFT) | RADIX_TREE_MAP_MASK;
        height_to_maxindex[i] = max;
    }
    inited = true;
}

static radix_tree_node_t *radix_node_alloc(uint8_t shift) {
    radix_tree_node_t *node = (radix_tree_node_t *)kmalloc(sizeof(radix_tree_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(radix_tree_node_t));
    node->shift = shift;
    return node;
}

static void radix_node_free(radix_tree_node_t *node) {
    if (node) kfree(node);
}

void radix_tree_init(radix_tree_root_t *root) {
    if (!root) return;
    init_maxindex();
    root->height = 0;
    root->rnode = NULL;
}

static int radix_tree_extend(radix_tree_root_t *root, uint64_t index) {
    init_maxindex();
    uint8_t height = root->height;
    while (index > height_to_maxindex[height]) {
        height++;
        if (height >= 8) return -1;
    }

    if (root->rnode == NULL) {
        root->height = height;
        return 0;
    }

    do {
        radix_tree_node_t *node = radix_node_alloc((uint8_t)(root->height * RADIX_TREE_MAP_SHIFT));
        if (!node) return -1;

        node->slots[0] = root->rnode;
        node->count = 1;

        for (int tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {
            if (root->height > 0) {
                radix_tree_node_t *old_root = (radix_tree_node_t *)root->rnode;
                if (old_root->tags[tag]) node->tags[tag] |= 1ULL;
            }
        }

        root->rnode = node;
        root->height++;
    } while (height > root->height);

    return 0;
}

int radix_tree_insert(radix_tree_root_t *root, uint64_t index, void *item) {
    if (!root || !item) return -1;
    init_maxindex();

    if (index > height_to_maxindex[root->height]) {
        if (radix_tree_extend(root, index) < 0) return -1;
    }

    if (root->height == 0) {
        if (root->rnode) return -1;
        root->rnode = item;
        return 0;
    }

    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);

    while (height > 1) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        if (!node->slots[slot]) {
            radix_tree_node_t *child = radix_node_alloc((uint8_t)(shift - RADIX_TREE_MAP_SHIFT));
            if (!child) return -1;
            node->slots[slot] = child;
            node->count++;
        }
        node = (radix_tree_node_t *)node->slots[slot];
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }

    unsigned int slot = (unsigned int)(index & RADIX_TREE_MAP_MASK);
    if (node->slots[slot]) return -1;

    node->slots[slot] = item;
    node->count++;
    return 0;
}

void *radix_tree_lookup(const radix_tree_root_t *root, uint64_t index) {
    if (!root || !root->rnode) return NULL;
    init_maxindex();
    if (index > height_to_maxindex[root->height]) return NULL;

    if (root->height == 0) {
        return (index == 0) ? root->rnode : NULL;
    }

    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);

    while (height > 0) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        if (!node->slots[slot]) return NULL;
        if (height == 1) return node->slots[slot];
        node = (radix_tree_node_t *)node->slots[slot];
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }
    return NULL;
}

void *radix_tree_delete(radix_tree_root_t *root, uint64_t index) {
    if (!root || !root->rnode) return NULL;
    init_maxindex();
    if (index > height_to_maxindex[root->height]) return NULL;

    if (root->height == 0) {
        if (index != 0) return NULL;
        void *ret = root->rnode;
        root->rnode = NULL;
        return ret;
    }

    radix_tree_node_t *path[8];
    unsigned int slot_path[8];
    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);
    int depth = 0;

    while (height > 0) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        path[depth] = node;
        slot_path[depth] = slot;
        depth++;

        if (!node->slots[slot]) return NULL;
        if (height == 1) break;

        node = (radix_tree_node_t *)node->slots[slot];
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }

    depth--;
    radix_tree_node_t *leaf = path[depth];
    unsigned int leaf_slot = slot_path[depth];
    void *item = leaf->slots[leaf_slot];
    if (!item) return NULL;

    leaf->slots[leaf_slot] = NULL;
    leaf->count--;
    for (int tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {
        leaf->tags[tag] &= ~(1ULL << leaf_slot);
    }

    while (depth >= 0) {
        radix_tree_node_t *cur = path[depth];
        unsigned int cur_slot = slot_path[depth];

        for (int tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {
            if (cur->tags[tag] & (1ULL << cur_slot)) {
                if (depth > 0) {
                    radix_tree_node_t *child = (radix_tree_node_t *)cur->slots[cur_slot];
                    if (!child || child->tags[tag] == 0) {
                        cur->tags[tag] &= ~(1ULL << cur_slot);
                    }
                }
            }
        }

        if (depth > 0 && cur->slots[cur_slot] && ((radix_tree_node_t *)cur->slots[cur_slot])->count == 0) {
            radix_node_free((radix_tree_node_t *)cur->slots[cur_slot]);
            cur->slots[cur_slot] = NULL;
            cur->count--;
        }
        depth--;
    }

    if (((radix_tree_node_t *)root->rnode)->count == 0) {
        radix_node_free((radix_tree_node_t *)root->rnode);
        root->rnode = NULL;
        root->height = 0;
    }

    return item;
}

void radix_tree_tag_set(radix_tree_root_t *root, uint64_t index, unsigned int tag) {
    if (!root || !root->rnode || tag >= RADIX_TREE_MAX_TAGS) return;
    init_maxindex();
    if (index > height_to_maxindex[root->height]) return;
    if (root->height == 0) return;

    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);

    while (height > 0) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        node->tags[tag] |= (1ULL << slot);
        if (height == 1) break;
        node = (radix_tree_node_t *)node->slots[slot];
        if (!node) return;
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }
}

void radix_tree_tag_clear(radix_tree_root_t *root, uint64_t index, unsigned int tag) {
    if (!root || !root->rnode || tag >= RADIX_TREE_MAX_TAGS) return;
    init_maxindex();
    if (index > height_to_maxindex[root->height]) return;
    if (root->height == 0) return;

    radix_tree_node_t *path[8];
    unsigned int slot_path[8];
    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);
    int depth = 0;

    while (height > 0) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        path[depth] = node;
        slot_path[depth] = slot;
        depth++;
        if (height == 1) break;
        node = (radix_tree_node_t *)node->slots[slot];
        if (!node) return;
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }

    depth--;
    while (depth >= 0) {
        radix_tree_node_t *cur = path[depth];
        unsigned int slot = slot_path[depth];
        if (depth == (int)root->height - 1) {
            cur->tags[tag] &= ~(1ULL << slot);
        } else {
            radix_tree_node_t *child = (radix_tree_node_t *)cur->slots[slot];
            if (!child || child->tags[tag] == 0) {
                cur->tags[tag] &= ~(1ULL << slot);
            } else {
                break;
            }
        }
        depth--;
    }
}

int radix_tree_tag_get(const radix_tree_root_t *root, uint64_t index, unsigned int tag) {
    if (!root || !root->rnode || tag >= RADIX_TREE_MAX_TAGS) return 0;
    init_maxindex();
    if (index > height_to_maxindex[root->height]) return 0;
    if (root->height == 0) return 0;

    radix_tree_node_t *node = (radix_tree_node_t *)root->rnode;
    uint8_t height = root->height;
    uint8_t shift = (uint8_t)((height - 1) * RADIX_TREE_MAP_SHIFT);

    while (height > 0) {
        unsigned int slot = (unsigned int)((index >> shift) & RADIX_TREE_MAP_MASK);
        if (!(node->tags[tag] & (1ULL << slot))) return 0;
        if (height == 1) return 1;
        node = (radix_tree_node_t *)node->slots[slot];
        if (!node) return 0;
        shift = (uint8_t)(shift - RADIX_TREE_MAP_SHIFT);
        height--;
    }
    return 0;
}

static size_t gang_lookup_node(radix_tree_node_t *node, void **results, uint64_t *cur_index, uint64_t first_index, size_t max_items, int tag) {
    size_t found = 0;
    if (!node || max_items == 0) return 0;

    uint8_t shift = node->shift;
    for (unsigned int i = 0; i < RADIX_TREE_MAP_SIZE; i++) {
        if (tag >= 0 && !(node->tags[tag] & (1ULL << i))) continue;
        if (!node->slots[i]) continue;

        uint64_t slot_base = (*cur_index & ~((1ULL << (shift + RADIX_TREE_MAP_SHIFT)) - 1)) | ((uint64_t)i << shift);
        if (shift == 0) {
            if (slot_base >= first_index) {
                results[found++] = node->slots[i];
                if (found >= max_items) return found;
            }
        } else {
            uint64_t slot_max = slot_base | ((1ULL << shift) - 1);
            if (slot_max >= first_index) {
                uint64_t child_cur = slot_base;
                found += gang_lookup_node((radix_tree_node_t *)node->slots[i], results + found, &child_cur, first_index, max_items - found, tag);
                if (found >= max_items) return found;
            }
        }
    }
    return found;
}

size_t radix_tree_gang_lookup(const radix_tree_root_t *root, void **results, uint64_t first_index, size_t max_items) {
    if (!root || !root->rnode || max_items == 0) return 0;
    if (root->height == 0) {
        if (first_index == 0) {
            results[0] = root->rnode;
            return 1;
        }
        return 0;
    }
    uint64_t cur = 0;
    return gang_lookup_node((radix_tree_node_t *)root->rnode, results, &cur, first_index, max_items, -1);
}

size_t radix_tree_gang_lookup_tag(const radix_tree_root_t *root, void **results, uint64_t first_index, size_t max_items, unsigned int tag) {
    if (!root || !root->rnode || max_items == 0 || tag >= RADIX_TREE_MAX_TAGS) return 0;
    if (root->height == 0) return 0;
    uint64_t cur = 0;
    return gang_lookup_node((radix_tree_node_t *)root->rnode, results, &cur, first_index, max_items, (int)tag);
}
