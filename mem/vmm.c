// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "vmm.h"
#include <stddef.h>

static vmm_space_t kernel_space;

void vmm_init(void) {
}

vmm_space_t *vmm_get_kernel_space(void) {
    return &kernel_space;
}

vmm_space_t *vmm_create_space(void) {
    return NULL;
}

void vmm_destroy_space(vmm_space_t *space) {
    (void)space;
}

vmm_space_t *vmm_clone_space(vmm_space_t *parent_space) {
    (void)parent_space;
    return NULL;
}

void *vmm_map(vmm_space_t *space, uintptr_t hint, size_t length, uint32_t prot, uint32_t flags, void *file, uint64_t offset) {
    (void)space; (void)hint; (void)length; (void)prot; (void)flags; (void)file; (void)offset;
    return NULL;
}

int vmm_unmap(vmm_space_t *space, uintptr_t addr, size_t length) {
    (void)space; (void)addr; (void)length;
    return 0;
}

int vmm_protect(vmm_space_t *space, uintptr_t addr, size_t length, uint32_t prot) {
    (void)space; (void)addr; (void)length; (void)prot;
    return 0;
}

int vmm_handle_page_fault(vmm_space_t *space, uintptr_t fault_addr, uint32_t error_code) {
    (void)space; (void)fault_addr; (void)error_code;
    return -1;
}
