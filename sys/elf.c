// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "elf.h"
#include "vfs.h"
#include "slab.h"
#include "kutils.h"

#include "mmu.h"
#include "platform.h"

extern void serial_print(const char *s);
extern void serial_write(const char *str);

uint64_t elf_load(const char *path, uint64_t user_pml4, size_t *out_load_size, struct process *proc,
                  uint64_t *out_phdr_vaddr, uint64_t *out_phdr_num) {
    if (out_load_size) *out_load_size = 0;
    vfs_file_t *file = vfs_open(path, "r");
    if (!file) {
        serial_write("[ELF] Error: Failed to open file ");
        serial_write(path);
        serial_write("\n");
        return 0;
    }

    // Read the ELF Header
    Elf64_Ehdr ehdr;
    if (vfs_read(file, &ehdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
        serial_write("[ELF] Error: Could not read ELF Header\n");
        vfs_close(file);
        return 0;
    }

    // Validate Magic Number & Properties
    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 || 
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        serial_write("[ELF] Error: Invalid ELF Magic Number\n");
        vfs_close(file);
        return 0;
    }
    if (ehdr.e_ident[4] != ELFCLASS64) {
        serial_write("[ELF] Error: Not a 64-bit ELF\n");
        vfs_close(file);
        return 0;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        serial_write("[ELF] Error: Not Little Endian\n");
        vfs_close(file);
        return 0;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        serial_write("[ELF] Error: Not an Executable\n");
        vfs_close(file);
        return 0;
    }
    if (ehdr.e_machine != EM_X86_64) {
        serial_write("[ELF] Error: Not x86_64 Architecture\n");
        vfs_close(file);
        return 0;
    }

    if (out_phdr_num) *out_phdr_num = ehdr.e_phnum;
    uint64_t first_load_vaddr = 0;
    uint64_t phoff = ehdr.e_phoff;

    // Iterate Over Program Headers
    for (int i = 0; i < ehdr.e_phnum; i++) {
        vfs_seek(file, ehdr.e_phoff + (i * ehdr.e_phentsize), 0);
        Elf64_Phdr phdr;
        if (vfs_read(file, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            serial_write("[ELF] Error: Failed to read Program Header\n");
            continue;
        }

        // Only load segments with type PT_LOAD
        if (phdr.p_type == PT_LOAD) {
            uint64_t p_vaddr = phdr.p_vaddr;
            uint64_t p_memsz = phdr.p_memsz;
            uint64_t p_filesz = phdr.p_filesz;
            uint64_t p_offset = phdr.p_offset;
            if (first_load_vaddr == 0) first_load_vaddr = p_vaddr;

            if (p_memsz == 0) continue;

            // Calculate boundaries for page allocation
            uintptr_t align_offset = p_vaddr & 0xFFF;
            uintptr_t start_page = p_vaddr & ~0xFFFULL;
            size_t total_needed = (p_memsz + align_offset + 4095) & ~4095ULL;
            size_t num_pages = total_needed / 4096;

            void* bulk_phys = kmalloc_aligned(total_needed, 4096);
            if (!bulk_phys) {
                serial_write("[ELF] Error: Out of memory bulk allocating segment\n");
                vfs_close(file);
                return 0;
            }

            // Zero out entire segment (BSS and padding) in one go
            memset(bulk_phys, 0, total_needed);

            // Bulk read from disk for the entire filesz part
            if (p_filesz > 0) {
                vfs_seek(file, p_offset, 0);
                vfs_read(file, (uint8_t*)bulk_phys + align_offset, (uint32_t)p_filesz);
            }

            // Map all pages
            for (uint64_t p = 0; p < num_pages; p++) {
                uint64_t vaddr = start_page + (p * 4096);
                uint64_t phys_addr = v2p((uint64_t)bulk_phys + (p * 4096));
                mmu_context_t ctx = { .pml4_phys = user_pml4, .lock = SPINLOCK_INIT };
                if (mmu_map_page(&ctx, vaddr, phys_addr, MMU_PROT_READ | MMU_PROT_WRITE | MMU_PROT_USER | MMU_PROT_EXEC) != 0)
                    return 0;
            }
            
            if (proc) {
                extern void process_add_elf_segment(struct process *proc, void *ptr, uint64_t vaddr, size_t size);
                process_add_elf_segment(proc, bulk_phys, start_page, total_needed);
            }

            if (out_load_size) *out_load_size += total_needed;
        }
    }

    vfs_close(file);
    if (out_phdr_vaddr) *out_phdr_vaddr = (first_load_vaddr & ~0xFFFULL) + phoff;
    return ehdr.e_entry;
}
