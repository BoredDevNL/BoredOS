// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "vma.h"
#include "slab.h"
#include <string.h>

#define PAGE_SIZE 4096UL
#define PAGE_MASK (PAGE_SIZE - 1)

static inline size_t max_gap(size_t a, size_t b) {
    return (a > b) ? a : b;
}

static inline size_t node_gap_to_next(const vm_area_t *node) {
    if (!node || !node->next) return 0;
    return (node->next->start > node->end) ? (node->next->start - node->end) : 0;
}

static void update_vma_subtree_gap(rb_node_t *rb_node) {
    if (!rb_node) return;
    vm_area_t *node = vma_from_rb(rb_node);

    size_t left_gap  = rb_node->rb_left  ? vma_from_rb(rb_node->rb_left)->rb_subtree_max_gap  : 0;
    size_t right_gap = rb_node->rb_right ? vma_from_rb(rb_node->rb_right)->rb_subtree_max_gap : 0;
    size_t self_gap  = node_gap_to_next(node);

    node->rb_subtree_max_gap = max_gap(self_gap, max_gap(left_gap, right_gap));
}

static void propagate_gap_upward(vm_area_t *node) {
    while (node) {
        update_vma_subtree_gap(&node->rb_node);
        node = node->rb_node.rb_parent ? vma_from_rb(node->rb_node.rb_parent) : NULL;
    }
}

vm_area_t *vma_create(uintptr_t start, uintptr_t end, uint32_t type, uint32_t flags) {
    uintptr_t aligned_start = start & ~PAGE_MASK;
    uintptr_t aligned_end = (end + PAGE_MASK) & ~PAGE_MASK;

    if (aligned_start >= aligned_end) return NULL;

    vm_area_t *vma = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    if (!vma) return NULL;

    memset(vma, 0, sizeof(vm_area_t));
    vma->start = aligned_start;
    vma->end = aligned_end;
    vma->type = type;
    vma->flags = flags;
    vma->rb_node.rb_color = RB_RED;

    return vma;
}

void vma_destroy(vm_area_t *vma) {
    if (!vma) return;
    kfree(vma);
}

vm_area_t *vma_find(const rb_root_t *root, uintptr_t addr) {
    if (!root || !root->rb_node) return NULL;
    rb_node_t *current = root->rb_node;

    while (current) {
        vm_area_t *vma = vma_from_rb(current);
        if (addr >= vma->start && addr < vma->end) {
            return vma;
        }
        if (addr < vma->start) {
            current = current->rb_left;
        } else {
            current = current->rb_right;
        }
    }
    return NULL;
}

uintptr_t vma_find_unmapped_area(const rb_root_t *root, uintptr_t min_addr, uintptr_t max_addr, size_t length, size_t alignment) {
    if (length == 0 || min_addr >= max_addr || length > (max_addr - min_addr)) {
        return 0;
    }

    if (alignment < PAGE_SIZE) alignment = PAGE_SIZE;
    size_t align_mask = alignment - 1;

    size_t aligned_length = (length + PAGE_MASK) & ~PAGE_MASK;
    uintptr_t aligned_min = (min_addr + align_mask) & ~align_mask;

    if (!root || !root->rb_node) {
        return (aligned_min + aligned_length <= max_addr) ? aligned_min : 0;
    }

    vm_area_t *curr = vma_from_rb(rb_first(root));
    if (!curr) return 0;

    if (curr->start >= aligned_min + aligned_length) {
        return aligned_min;
    }

    while (curr) {
        uintptr_t candidate = (curr->end + align_mask) & ~align_mask;
        uintptr_t gap_end = curr->next ? curr->next->start : max_addr;

        if (candidate < aligned_min) candidate = aligned_min;
        if (gap_end > max_addr) gap_end = max_addr;

        if (gap_end >= candidate && (gap_end - candidate) >= aligned_length) {
            return candidate;
        }
        curr = curr->next;
    }

    return 0;
}

bool vma_insert(vm_area_t **head, rb_root_t *root, vm_area_t *vma) {
    if (!head || !root || !vma) return false;

    rb_node_t **link = &root->rb_node;
    rb_node_t *parent = NULL;
    vm_area_t *prev_node = NULL;

    while (*link) {
        parent = *link;
        vm_area_t *curr = vma_from_rb(parent);

        if (!(vma->end <= curr->start || vma->start >= curr->end)) {
            return false;
        }

        if (vma->end <= curr->start) {
            link = &parent->rb_left;
        } else {
            prev_node = curr;
            link = &parent->rb_right;
        }
    }

    rb_link_node(&vma->rb_node, parent, link);

    if (!prev_node) {
        vma->prev = NULL;
        vma->next = *head;
        if (*head) (*head)->prev = vma;
        *head = vma;
    } else {
        vma->prev = prev_node;
        vma->next = prev_node->next;
        if (prev_node->next) prev_node->next->prev = vma;
        prev_node->next = vma;
    }

    if (vma->prev) propagate_gap_upward(vma->prev);
    propagate_gap_upward(vma);

    rb_insert_augmented(root, &vma->rb_node, update_vma_subtree_gap);
    propagate_gap_upward(vma);

    return true;
}

bool vma_remove(vm_area_t **head, rb_root_t *root, vm_area_t *vma) {
    if (!head || !root || !vma || !root->rb_node) return false;

    vm_area_t *prev = vma->prev;
    vm_area_t *next = vma->next;

    if (prev) prev->next = next;
    else *head = next;

    if (next) next->prev = prev;

    rb_erase_augmented(root, &vma->rb_node, update_vma_subtree_gap);

    if (prev) propagate_gap_upward(prev);
    return true;
}

static inline bool vma_can_merge(const vm_area_t *first, const vm_area_t *second) {
    if (!first || !second) return false;
    if (first->end != second->start) return false;
    if (first->type != second->type) return false;
    if (first->flags != second->flags) return false;
    if (first->backing_file != second->backing_file) return false;

    if (first->type == VMA_TYPE_FILE) {
        size_t first_len = first->end - first->start;
        if (first->file_offset + first_len != second->file_offset) {
            return false;
        }
    }
    return true;
}

vm_area_t *vma_merge_adjacent(vm_area_t **head, rb_root_t *root, vm_area_t *prev, vm_area_t *next) {
    if (!head || !root || !prev || !next) return NULL;
    if (!vma_can_merge(prev, next)) return NULL;

    prev->end = next->end;
    vma_remove(head, root, next);
    vma_destroy(next);

    propagate_gap_upward(prev);
    return prev;
}

bool vma_split(vm_area_t *vma, uintptr_t split_addr, vm_area_t **out_left, vm_area_t **out_right) {
    if (!vma || !out_left || !out_right) return false;

    uintptr_t aligned_split = split_addr & ~PAGE_MASK;
    if (aligned_split <= vma->start || aligned_split >= vma->end) {
        return false;
    }

    vm_area_t *right = vma_create(aligned_split, vma->end, vma->type, vma->flags);
    if (!right) return false;

    right->backing_file = vma->backing_file;
    if (vma->type == VMA_TYPE_FILE) {
        right->file_offset = vma->file_offset + (aligned_split - vma->start);
    }

    vma->end = aligned_split;
    propagate_gap_upward(vma);

    *out_left = vma;
    *out_right = right;
    return true;
}

bool vma_verify_invariants(const rb_root_t *root) {
    if (!root || !root->rb_node) return true;
    if (!rb_verify(root)) return false;

    rb_node_t *node = rb_first(root);
    vm_area_t *prev = NULL;
    while (node) {
        vm_area_t *curr = vma_from_rb(node);
        if (curr->start >= curr->end) return false;
        if (prev && prev->end > curr->start) return false;
        prev = curr;
        node = rb_next(node);
    }
    return true;
}
