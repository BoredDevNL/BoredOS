// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
/*
################################################################

BEWARE

THIS IS SCAFFOLDING, CAN VERY MUCH BE CHANGED!!

#################################################################

*/

#ifndef BOREDOS_VMA_H
#define BOREDOS_VMA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

#define VMA_COLOR_RED       0
#define VMA_COLOR_BLACK     1

typedef struct vm_area {
    uintptr_t start;
    uintptr_t end;
    uint32_t type;
    uint32_t flags;
    void *backing_file;
    uint64_t file_offset;
    
    struct vm_area *prev;
    struct vm_area *next;
    
    struct vm_area *rb_left;
    struct vm_area *rb_right;
    struct vm_area *rb_parent;
    uint32_t rb_color;
} vm_area_t;

vm_area_t *vma_create(uintptr_t start, uintptr_t end, uint32_t type, uint32_t flags);
void vma_destroy(vm_area_t *vma);
vm_area_t *vma_find(vm_area_t *root, uintptr_t addr);
bool vma_insert(vm_area_t **head, vm_area_t **root, vm_area_t *vma);
bool vma_remove(vm_area_t **head, vm_area_t **root, vm_area_t *vma);
bool vma_split(vm_area_t *vma, uintptr_t split_addr, vm_area_t **out_left, vm_area_t **out_right);

#endif // BOREDOS_VMA_H
