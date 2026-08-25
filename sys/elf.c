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
extern void process_add_elf_segment(struct process *proc, void *ptr, uint64_t vaddr, size_t size);

#define INTERP_LOAD_BASE 0x70000000ULL
#define PIE_LOAD_BASE    0x40000000ULL

static bool load_elf_segments(vfs_file_t *file, const Elf64_Ehdr *ehdr, uint64_t base_vaddr,
                              uint64_t user_pml4, struct process *proc, size_t *out_load_size,
                              uint64_t *out_first_load_vaddr) {
    if (out_first_load_vaddr) *out_first_load_vaddr = 0;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        vfs_seek(file, ehdr->e_phoff + (i * ehdr->e_phentsize), 0);
        Elf64_Phdr phdr;
        if (vfs_read(file, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            serial_write("[ELF] Error: Failed to read Program Header\n");
            return false;
        }

        if (phdr.p_type == PT_LOAD) {
            uint64_t p_vaddr = base_vaddr + phdr.p_vaddr;
            uint64_t p_memsz = phdr.p_memsz;
            uint64_t p_filesz = phdr.p_filesz;
            uint64_t p_offset = phdr.p_offset;

            if (out_first_load_vaddr && *out_first_load_vaddr == 0) {
                *out_first_load_vaddr = p_vaddr;
            }

            if (p_memsz == 0) continue;

            uintptr_t align_offset = p_vaddr & 0xFFF;
            uintptr_t start_page = p_vaddr & ~0xFFFULL;
            size_t total_needed = (p_memsz + align_offset + 4095) & ~4095ULL;
            size_t num_pages = total_needed / 4096;

            void *bulk_phys = kmalloc_aligned(total_needed, 4096);
            if (!bulk_phys) {
                serial_write("[ELF] Error: Out of memory allocating segment\n");
                return false;
            }

            memset(bulk_phys, 0, total_needed);

            if (p_filesz > 0) {
                vfs_seek(file, p_offset, 0);
                vfs_read(file, (uint8_t *)bulk_phys + align_offset, (uint32_t)p_filesz);
            }

            uint32_t map_flags = MMU_PROT_READ | MMU_PROT_USER;
            if (phdr.p_flags & PF_W) {
                map_flags |= MMU_PROT_WRITE;
            }
            if (phdr.p_flags & PF_X) {
                map_flags |= MMU_PROT_EXEC;
            }

            mmu_context_t ctx = { .pml4_phys = user_pml4, .lock = SPINLOCK_INIT };
            for (uint64_t p = 0; p < num_pages; p++) {
                uint64_t vaddr = start_page + (p * 4096);
                uint64_t phys_addr = v2p((uint64_t)bulk_phys + (p * 4096));
                if (mmu_map_page(&ctx, vaddr, phys_addr, map_flags) != 0) {
                    kfree_null(bulk_phys);
                    return false;
                }
            }

            if (proc) {
                process_add_elf_segment(proc, bulk_phys, start_page, total_needed);
            }

            if (out_load_size) *out_load_size += total_needed;
        }
    }
    return true;
}

bool elf_load(const char *path, uint64_t user_pml4, struct process *proc, elf_load_result_t *out_result) {
    if (!out_result) return false;
    memset(out_result, 0, sizeof(elf_load_result_t));

    vfs_file_t *file = vfs_open(path, "r");
    if (!file) {
        serial_write("[ELF] Error: Failed to open file ");
        serial_write(path);
        serial_write("\n");
        return false;
    }

    Elf64_Ehdr ehdr;
    if (vfs_read(file, &ehdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
        serial_write("[ELF] Error: Could not read ELF Header\n");
        vfs_close(file);
        return false;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 || 
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        serial_write("[ELF] Error: Invalid ELF Magic Number\n");
        vfs_close(file);
        return false;
    }
    if (ehdr.e_ident[4] != ELFCLASS64) {
        serial_write("[ELF] Error: Not a 64-bit ELF\n");
        vfs_close(file);
        return false;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        serial_write("[ELF] Error: Not Little Endian\n");
        vfs_close(file);
        return false;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        serial_write("[ELF] Error: Not an Executable or Shared Object\n");
        vfs_close(file);
        return false;
    }
    if (ehdr.e_machine != EM_X86_64) {
        serial_write("[ELF] Error: Not x86_64 Architecture\n");
        vfs_close(file);
        return false;
    }

    uint64_t exec_base = (ehdr.e_type == ET_DYN) ? PIE_LOAD_BASE : 0;
    char interp_path[256];
    interp_path[0] = '\0';
    uint64_t pt_phdr_vaddr = 0;

    // Scan program headers for PT_INTERP and PT_PHDR
    for (int i = 0; i < ehdr.e_phnum; i++) {
        vfs_seek(file, ehdr.e_phoff + (i * ehdr.e_phentsize), 0);
        Elf64_Phdr phdr;
        if (vfs_read(file, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            continue;
        }

        if (phdr.p_type == PT_INTERP) {
            size_t len = (phdr.p_filesz < sizeof(interp_path) - 1) ? phdr.p_filesz : (sizeof(interp_path) - 1);
            vfs_seek(file, phdr.p_offset, 0);
            vfs_read(file, interp_path, (uint32_t)len);
            interp_path[len] = '\0';
        } else if (phdr.p_type == PT_PHDR) {
            pt_phdr_vaddr = exec_base + phdr.p_vaddr;
        }
    }

    uint64_t first_load_vaddr = 0;
    size_t total_load_size = 0;
    if (!load_elf_segments(file, &ehdr, exec_base, user_pml4, proc, &total_load_size, &first_load_vaddr)) {
        vfs_close(file);
        return false;
    }
    vfs_close(file);

    uint64_t exec_entry = exec_base + ehdr.e_entry;
    uint64_t phdr_vaddr = pt_phdr_vaddr;
    if (phdr_vaddr == 0) {
        phdr_vaddr = (first_load_vaddr != 0) ? (first_load_vaddr & ~0xFFFULL) + ehdr.e_phoff : (exec_base + ehdr.e_phoff);
    }

    out_result->exec_entry = exec_entry;
    out_result->phdr_vaddr = phdr_vaddr;
    out_result->phdr_num = ehdr.e_phnum;
    out_result->load_size = total_load_size;

    if (interp_path[0] != '\0') {
        vfs_file_t *interp_file = vfs_open(interp_path, "r");
        if (!interp_file) {
            if (strcmp(interp_path, "/lib/ld.so") != 0 && strcmp(interp_path, "/usr/lib/ld.so") != 0) {
                interp_file = vfs_open("/usr/lib/ld.so", "r");
                if (!interp_file) {
                    interp_file = vfs_open("/lib/ld.so", "r");
                }
            }
        }

        if (!interp_file) {
            serial_write("[ELF] Error: Failed to open dynamic linker: ");
            serial_write(interp_path);
            serial_write("\n");
            return false;
        }

        Elf64_Ehdr interp_ehdr;
        if (vfs_read(interp_file, &interp_ehdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr) ||
            interp_ehdr.e_ident[0] != ELFMAG0 || interp_ehdr.e_ident[1] != ELFMAG1 ||
            interp_ehdr.e_ident[2] != ELFMAG2 || interp_ehdr.e_ident[3] != ELFMAG3) {
            serial_write("[ELF] Error: Invalid dynamic linker ELF\n");
            vfs_close(interp_file);
            return false;
        }

        uint64_t interp_base = (interp_ehdr.e_type == ET_DYN) ? INTERP_LOAD_BASE : 0;
        size_t interp_load_size = 0;
        if (!load_elf_segments(interp_file, &interp_ehdr, interp_base, user_pml4, proc, &interp_load_size, NULL)) {
            serial_write("[ELF] Error: Failed to load dynamic linker segments\n");
            vfs_close(interp_file);
            return false;
        }
        vfs_close(interp_file);

        out_result->load_size += interp_load_size;
        out_result->interp_base = interp_base;
        out_result->entry_point = interp_base + interp_ehdr.e_entry;
        out_result->has_interp = true;
    } else {
        out_result->interp_base = 0;
        out_result->entry_point = exec_entry;
        out_result->has_interp = false;
    }

    return true;
}

