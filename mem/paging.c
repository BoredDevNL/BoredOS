// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "paging.h"
#include "memory_manager.h"
#include "platform.h"

#include "../graphics/graphics.h"
#include "../core/msrs.h"
#include <stddef.h>

#define MSR_WC  0x277

static uint64_t current_pml4_phys = 0;
static uint64_t kernel_pml4_phys = 0;

// Get current CR3 value
static uint64_t read_cr3(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Set CR3 value
static void write_cr3(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" : : "r"(cr3));
}

// Helper to allocate a page table and clear it
static uint64_t alloc_page_table_phys(void) {
    void *ptr = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    if (!ptr) return 0;
    
    page_table_t* table = (page_table_t*)ptr;
    
    // Clear table 
    for (int i = 0; i < 512; i++) {
        table->entries[i] = 0;
    }
    
    // Return the physical address of this table
    return v2p((uint64_t)table);
}

static bool paging_map_page_2m(uint64_t pml4_phys, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if (!pml4_phys) return false;

    page_table_t* pml4 = (page_table_t*)p2v(pml4_phys);

    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index   = (virtual_addr >> 21) & 0x1FF;

    if (!(pml4->entries[pml4_index] & PT_PRESENT)) {
        uint64_t new_table_phys = alloc_page_table_phys();
        if (!new_table_phys) return false;
        pml4->entries[pml4_index] = new_table_phys | PT_PRESENT | PT_RW | PT_USER;
    }

    page_table_t* pdpt = (page_table_t*)p2v(pml4->entries[pml4_index] & PT_ADDR_MASK);
    if (!(pdpt->entries[pdpt_index] & PT_PRESENT)) {
        uint64_t new_table_phys = alloc_page_table_phys();
        if (!new_table_phys) return false;
        pdpt->entries[pdpt_index] = new_table_phys | PT_PRESENT | PT_RW | PT_USER;
    }

    page_table_t* pd = (page_table_t*)p2v(pdpt->entries[pdpt_index] & PT_ADDR_MASK);
    pd->entries[pd_index] = (physical_addr & PT_ADDR_MASK) | flags;

    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return true;
}

/// @brief Enables write combining in the model specific registers
void pat_enable_wc(void) {
    uint64_t pat = rdmsr(0x277);

    pat &= ~(0xFFULL << 32);
    pat |=  (0x01ULL << 32);

    wrmsr(0x277, pat);

    asm volatile("mov %%cr3, %%rax\n"
                "mov %%rax, %%cr3\n"
                ::: "rax", "memory");
}

void paging_init(void) {
    pat_enable_wc();

    uintptr_t fb_base = (uintptr_t)graphics_get_fb_addr();
    size_t fb_size =
        get_screen_height() * graphics_get_fb_pitch();
    uintptr_t fb_end = fb_base + fb_size;

    uintptr_t fb_map_base = fb_base & ~(PAGE_SIZE_2M - 1);
    uintptr_t fb_map_end = (fb_end + PAGE_SIZE_2M - 1) & ~(PAGE_SIZE_2M - 1);
    uintptr_t cr3 = read_cr3();

    // Maps with specific flags for framebuffer optimisations, along with write combining this is ideal
    for (uintptr_t p = fb_map_base; p < fb_map_end; p += PAGE_SIZE_2M) {
        paging_map_page_2m(
            cr3,
            (uintptr_t)p2v(p),
            p,
            PT_PRESENT | PT_RW | PT_PAT | PT_NX | PT_GLOBAL | PT_HUGE
        );
    }

    kernel_pml4_phys = read_cr3() & PT_ADDR_MASK;
    current_pml4_phys = kernel_pml4_phys;
}

uint64_t paging_get_pml4_phys(void) {
    return read_cr3() & PT_ADDR_MASK;
}

uint64_t paging_get_kernel_pml4_phys(void) {
    return kernel_pml4_phys;
}

void paging_switch_directory(uint64_t pml4_phys) {
    current_pml4_phys = pml4_phys;
    write_cr3(pml4_phys);
}

bool paging_map_page(uint64_t pml4_phys, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if (!pml4_phys) return false;

    page_table_t* pml4 = (page_table_t*)p2v(pml4_phys);

    // Extract indices
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    uint64_t pd_index   = (virtual_addr >> 21) & 0x1FF;
    uint64_t pt_index   = (virtual_addr >> 12) & 0x1FF;

    // Check PML4 entry
    if (!(pml4->entries[pml4_index] & PT_PRESENT)) {
        uint64_t new_table_phys = alloc_page_table_phys();
        if (!new_table_phys) return false;
        pml4->entries[pml4_index] = new_table_phys | PT_PRESENT | PT_RW | PT_USER;
    }

    // Get PDPT
    page_table_t* pdpt = (page_table_t*)p2v(pml4->entries[pml4_index] & PT_ADDR_MASK);
    if (!(pdpt->entries[pdpt_index] & PT_PRESENT)) {
        uint64_t new_table_phys = alloc_page_table_phys();
        if (!new_table_phys) return false;
        pdpt->entries[pdpt_index] = new_table_phys | PT_PRESENT | PT_RW | PT_USER;
    }

    // Get PD
    page_table_t* pd = (page_table_t*)p2v(pdpt->entries[pdpt_index] & PT_ADDR_MASK);
    if (!(pd->entries[pd_index] & PT_PRESENT)) {
        uint64_t new_table_phys = alloc_page_table_phys();
        if (!new_table_phys) return false;
        pd->entries[pd_index] = new_table_phys | PT_PRESENT | PT_RW | PT_USER;
    }

    // Get PT
    page_table_t* pt = (page_table_t*)p2v(pd->entries[pd_index] & PT_ADDR_MASK);

    // Set entry in PT. Always update the entry so overlapping ELF LOAD segments
    // can overwrite earlier mappings for the same page.
    pt->entries[pt_index] = (physical_addr & PT_ADDR_MASK) | flags;

    // Flush TLB for this address
    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return true;
}

uint64_t paging_create_user_pml4_phys(void) {
    // 1. Allocate a new physical PML4
    uint64_t new_pml4_phys = alloc_page_table_phys();
    if (!new_pml4_phys) return 0;
    
    page_table_t* new_pml4 = (page_table_t*)p2v(new_pml4_phys);
    
    // 2. Clone the higher-half kernel mappings from the boot kernel PML4
    // In x86_64, indices 256-511 are the higher half.
    page_table_t* kernel_pml4 = (page_table_t*)p2v(kernel_pml4_phys);
    for (int i = 256; i < 512; i++) {
        new_pml4->entries[i] = kernel_pml4->entries[i];
    }
    
    // The lower half (0-255) is left empty for the user process to use
    return new_pml4_phys;
}

void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped_pages) {
    if (!pml4_phys) return;
    page_table_t* pml4 = (page_table_t*)p2v(pml4_phys);
    
    // Only traverse lower half (user space, indices 0-255)
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (pml4->entries[pml4_idx] & PT_PRESENT) {
            page_table_t* pdpt = (page_table_t*)p2v(pml4->entries[pml4_idx] & PT_ADDR_MASK);
            
            for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
                if (pdpt->entries[pdpt_idx] & PT_PRESENT) {
                    page_table_t* pd = (page_table_t*)p2v(pdpt->entries[pdpt_idx] & PT_ADDR_MASK);
                    
                    for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                        if (pd->entries[pd_idx] & PT_PRESENT) {
                            if (!(pd->entries[pd_idx] & PT_HUGE)) {
                                page_table_t* pt = (page_table_t*)p2v(pd->entries[pd_idx] & PT_ADDR_MASK);
                                
                                if (free_mapped_pages) {
                                    for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                                        if (pt->entries[pt_idx] & PT_PRESENT) {
                                            uint64_t phys = pt->entries[pt_idx] & PT_ADDR_MASK;
                                            extern bool mm_is_heap_address(void *ptr);
                                            void *phys_ptr = (void *)p2v(phys);
                                            if (mm_is_heap_address(phys_ptr)) {
                                                kfree_null(phys_ptr);
                                            }
                                        }
                                    }
                                }
                                
                                void *pt_ptr = (void *)pt;
                                kfree_null(pt_ptr);
                            }
                        }
                    }
                    void *pd_ptr = (void *)pd;
                    kfree_null(pd_ptr);
                }
            }
            void *pdpt_ptr = (void *)pdpt;
            kfree_null(pdpt_ptr);
        }
    }
    // Finally free the pml4 itself
    void *plm4_ptr = (void *)pml4;
    kfree_null(plm4_ptr);
}

uint64_t paging_virt2phys(uint64_t pml4_phys, uint64_t virtual_addr) {
    if (!pml4_phys) return 0;
    
    if (virtual_addr >= 0xFFFF800000000000ULL) {
        return v2p(virtual_addr);
    }
    
    page_table_t* pml4 = (page_table_t*)p2v(pml4_phys);
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    if (!(pml4->entries[pml4_index] & PT_PRESENT)) return 0;
    
    page_table_t* pdpt = (page_table_t*)p2v(pml4->entries[pml4_index] & PT_ADDR_MASK);
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    if (!(pdpt->entries[pdpt_index] & PT_PRESENT)) return 0;
    
    page_table_t* pd = (page_table_t*)p2v(pdpt->entries[pdpt_index] & PT_ADDR_MASK);
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    if (!(pd->entries[pd_index] & PT_PRESENT)) return 0;
    
    if (pd->entries[pd_index] & PT_HUGE) {
        return (pd->entries[pd_index] & PT_ADDR_MASK) | (virtual_addr & (PAGE_SIZE_2M - 1));
    }
    
    page_table_t* pt = (page_table_t*)p2v(pd->entries[pd_index] & PT_ADDR_MASK);
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    if (!(pt->entries[pt_index] & PT_PRESENT)) return 0;
    
    return (pt->entries[pt_index] & PT_ADDR_MASK) | (virtual_addr & 0xFFF);
}

void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr) {
    if (!pml4_phys) return;
    
    page_table_t* pml4 = (page_table_t*)p2v(pml4_phys);
    uint64_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    if (!(pml4->entries[pml4_index] & PT_PRESENT)) return;
    
    page_table_t* pdpt = (page_table_t*)p2v(pml4->entries[pml4_index] & PT_ADDR_MASK);
    uint64_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    if (!(pdpt->entries[pdpt_index] & PT_PRESENT)) return;
    
    page_table_t* pd = (page_table_t*)p2v(pdpt->entries[pdpt_index] & PT_ADDR_MASK);
    uint64_t pd_index = (virtual_addr >> 21) & 0x1FF;
    if (!(pd->entries[pd_index] & PT_PRESENT)) return;
    
    if (pd->entries[pd_index] & PT_HUGE) {
        pd->entries[pd_index] = 0;
        asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
        return;
    }
    
    page_table_t* pt = (page_table_t*)p2v(pd->entries[pd_index] & PT_ADDR_MASK);
    uint64_t pt_index = (virtual_addr >> 12) & 0x1FF;
    if (!(pt->entries[pt_index] & PT_PRESENT)) return;
    
    pt->entries[pt_index] = 0;
    asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

uint64_t paging_clone_user_pml4(uint64_t parent_pml4_phys) {
    uint64_t child_pml4_phys = paging_create_user_pml4_phys();
    if (!child_pml4_phys) return 0;

    page_table_t* parent_pml4 = (page_table_t*)p2v(parent_pml4_phys);
    page_table_t* child_pml4 = (page_table_t*)p2v(child_pml4_phys);

    // Copy user space mapping entries (0-255)
    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (parent_pml4->entries[pml4_idx] & PT_PRESENT) {
            uint64_t child_pdpt_phys = alloc_page_table_phys();
            if (!child_pdpt_phys) goto fail;
            child_pml4->entries[pml4_idx] = child_pdpt_phys | (parent_pml4->entries[pml4_idx] & ~PT_ADDR_MASK);

            page_table_t* parent_pdpt = (page_table_t*)p2v(parent_pml4->entries[pml4_idx] & PT_ADDR_MASK);
            page_table_t* child_pdpt = (page_table_t*)p2v(child_pdpt_phys);

            for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
                if (parent_pdpt->entries[pdpt_idx] & PT_PRESENT) {
                    uint64_t child_pd_phys = alloc_page_table_phys();
                    if (!child_pd_phys) goto fail;
                    child_pdpt->entries[pdpt_idx] = child_pd_phys | (parent_pdpt->entries[pdpt_idx] & ~PT_ADDR_MASK);

                    page_table_t* parent_pd = (page_table_t*)p2v(parent_pdpt->entries[pdpt_idx] & PT_ADDR_MASK);
                    page_table_t* child_pd = (page_table_t*)p2v(child_pd_phys);

                    for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                        if (parent_pd->entries[pd_idx] & PT_PRESENT) {
                            if (parent_pd->entries[pd_idx] & PT_HUGE) {
                                child_pd->entries[pd_idx] = parent_pd->entries[pd_idx];
                                continue;
                            }

                            uint64_t child_pt_phys = alloc_page_table_phys();
                            if (!child_pt_phys) goto fail;
                            child_pd->entries[pd_idx] = child_pt_phys | (parent_pd->entries[pd_idx] & ~PT_ADDR_MASK);

                            page_table_t* parent_pt = (page_table_t*)p2v(parent_pd->entries[pd_idx] & PT_ADDR_MASK);
                            page_table_t* child_pt = (page_table_t*)p2v(child_pt_phys);

                            for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                                if (parent_pt->entries[pt_idx] & PT_PRESENT) {
                                    uint64_t parent_phys = parent_pt->entries[pt_idx] & PT_ADDR_MASK;
                                    uint64_t flags = parent_pt->entries[pt_idx] & ~PT_ADDR_MASK;
                                    
                                    // Check if this physical frame belongs to standard RAM/heap
                                    extern bool mm_is_heap_address(void *ptr);
                                    if (mm_is_heap_address((void*)p2v(parent_phys))) {
                                        // Allocate a clean page for the child and copy content
                                        void *child_page = kmalloc_aligned(4096, 4096);
                                        if (!child_page) goto fail;
                                        extern void *memcpy(void *dest, const void *src, size_t n);
                                        memcpy(child_page, (void*)p2v(parent_phys), 4096);
                                        child_pt->entries[pt_idx] = v2p((uint64_t)child_page) | flags;
                                    } else {
                                        // Device mapping (like framebuffer), just share the physical address
                                        child_pt->entries[pt_idx] = parent_phys | flags;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return child_pml4_phys;

fail:
    paging_destroy_user_pml4_phys(child_pml4_phys, true);
    return 0;
}
