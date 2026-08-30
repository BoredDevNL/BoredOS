// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef BOREDOS_VMA_H
#define BOREDOS_VMA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "rbtree.h"

#define VMA_TYPE_ANON       0x01
#define VMA_TYPE_FILE       0x02
#define VMA_TYPE_DEVICE     0x03
#define VMA_TYPE_SHM        0x04

#define VMA_FLAG_READ       0x01
#define VMA_FLAG_WRITE      0x02
#define VMA_FLAG_EXEC       0x04
#define VMA_FLAG_SHARED     0x08
#define VMA_FLAG_COW        0x10
#define VMA_FLAG_STACK      0x20
#define VMA_FLAG_HEAP       0x40

struct vmm_space;

typedef struct vm_area {
    uintptr_t start;
    uintptr_t end;
    uint32_t type;
    uint32_t flags;
    void *backing_file;
    uint64_t file_offset;
    struct vmm_space *space;
    struct vm_area *i_mmap_next;

    struct vm_area *prev;
    struct vm_area *next;

    rb_node_t rb_node;
    size_t rb_subtree_max_gap;
} vm_area_t;

#define vma_from_rb(node) rb_entry(node, vm_area_t, rb_node)

vm_area_t *vma_create(uintptr_t start, uintptr_t end, uint32_t type, uint32_t flags);
void vma_destroy(vm_area_t *vma);

vm_area_t *vma_find(const rb_root_t *root, uintptr_t addr);
uintptr_t vma_find_unmapped_area(const rb_root_t *root, uintptr_t min_addr, uintptr_t max_addr, size_t length, size_t alignment);

bool vma_insert(vm_area_t **head, rb_root_t *root, vm_area_t *vma);
bool vma_remove(vm_area_t **head, rb_root_t *root, vm_area_t *vma);

vm_area_t *vma_merge_adjacent(vm_area_t **head, rb_root_t *root, vm_area_t *prev, vm_area_t *next);
bool vma_split(vm_area_t *vma, uintptr_t split_addr, vm_area_t **out_left, vm_area_t **out_right);

bool vma_verify_invariants(const rb_root_t *root);

#endif // BOREDOS_VMA_H
