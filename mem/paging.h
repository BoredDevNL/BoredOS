// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>

#define PAGE_SIZE 4096

// Page Table Entry Flags
#define PT_PRESENT      (1ull << 0)
#define PT_RW           (1ull << 1)
#define PT_USER         (1ull << 2)
#define PT_WRITE_THROUGH (1ull << 3)
#define PT_CACHE_DISABLE (1ull << 4)
#define PT_ACCESSED     (1ull << 5)
#define PT_DIRTY        (1ull << 6)
#define PT_HUGE         (1ull << 7)
#define PT_GLOBAL       (1ull << 8)
#define PT_NX           (1ull << 63)

#define PT_ADDR_MASK    0x000FFFFFFFFFF000ull

typedef struct {
    uint64_t entries[512];
} __attribute__((aligned(PAGE_SIZE))) page_table_t;

uint64_t paging_get_pml4_phys(void);

void paging_map_page(uint64_t pml4_phys, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

uint64_t paging_create_user_pml4_phys(void);

void paging_switch_directory(uint64_t pml4_phys);

// Destroys a user page directory, reclaiming all physical memory used for page table structures.
void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped_pages);
uint64_t paging_get_kernel_pml4_phys(void);

void paging_init(void);
uint64_t paging_virt2phys(uint64_t pml4_phys, uint64_t virtual_addr);
uint64_t paging_clone_user_pml4(uint64_t parent_pml4_phys);
void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);

#endif // PAGING_H
