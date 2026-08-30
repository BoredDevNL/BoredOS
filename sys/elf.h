// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef ELF_H
#define ELF_H

#include <stdint.h>

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT]; /* Magic number and other info */
    Elf64_Half    e_type;             /* Object file type */
    Elf64_Half    e_machine;          /* Architecture */
    Elf64_Word    e_version;          /* Object file version */
    Elf64_Addr    e_entry;            /* Entry point virtual address */
    Elf64_Off     e_phoff;            /* Program header table file offset */
    Elf64_Off     e_shoff;            /* Section header table file offset */
    Elf64_Word    e_flags;            /* Processor-specific flags */
    Elf64_Half    e_ehsize;           /* ELF header size in bytes */
    Elf64_Half    e_phentsize;        /* Program header table entry size */
    Elf64_Half    e_phnum;            /* Program header table entry count */
    Elf64_Half    e_shentsize;        /* Section header table entry size */
    Elf64_Half    e_shnum;            /* Section header table entry count */
    Elf64_Half    e_shstrndx;         /* Section header string table index */
} Elf64_Ehdr;

typedef struct {
    Elf64_Word    p_type;             /* Segment type */
    Elf64_Word    p_flags;            /* Segment flags */
    Elf64_Off     p_offset;           /* Segment file offset */
    Elf64_Addr    p_vaddr;            /* Segment virtual address */
    Elf64_Addr    p_paddr;            /* Segment physical address */
    Elf64_Xword   p_filesz;           /* Segment size in file */
    Elf64_Xword   p_memsz;            /* Segment size in memory */
    Elf64_Xword   p_align;            /* Segment alignment */
} Elf64_Phdr;

typedef struct {
    Elf64_Word    sh_name;            /* Section name (string tbl index) */
    Elf64_Word    sh_type;            /* Section type */
    Elf64_Xword   sh_flags;           /* Section flags */
    Elf64_Addr    sh_addr;            /* Section virtual addr at execution */
    Elf64_Off     sh_offset;          /* Section file offset */
    Elf64_Xword   sh_size;            /* Section size in bytes */
    Elf64_Word    sh_link;            /* Link to another section */
    Elf64_Word    sh_info;            /* Additional section information */
    Elf64_Xword   sh_addralign;       /* Section alignment */
    Elf64_Xword   sh_entsize;         /* Entry size if section holds table */
} Elf64_Shdr;

typedef struct {
    Elf64_Word n_namesz;              /* Name size in bytes */
    Elf64_Word n_descsz;              /* Descriptor size in bytes */
    Elf64_Word n_type;                /* Note type */
} Elf64_Nhdr;

/* e_ident constants */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

/* e_type constants */
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine constants */
#define EM_X86_64 62

/* p_type constants */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

/* AUXV (Auxiliary Vector) types */
#define AT_NULL    0
#define AT_IGNORE  1
#define AT_EXECFD  2
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_FLAGS   8
#define AT_ENTRY   9
#define AT_NOTELF  10
#define AT_UID     11
#define AT_EUID    12
#define AT_GID     13
#define AT_EGID    14
#define AT_PLATFORM 15
#define AT_HWCAP   16
#define AT_CLKTCK  17
#define AT_SECURE  23
#define AT_BASE_PLATFORM 24
#define AT_RANDOM  25
#define AT_HWCAP2  26
#define AT_EXECFN  31
#define AT_SYSINFO_EHDR 33

/* sh_type constants */
#define SHT_NOTE 7

/* p_flags constants */
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* BoredOS app metadata note constants */
#define BOREDOS_APP_NOTE_OWNER "BOREDOS"
#define BOREDOS_APP_NOTE_NAME BOREDOS_APP_NOTE_OWNER
#define BOREDOS_APP_NOTE_SECTION ".note.boredos.app"
#define BOREDOS_APP_NOTE_TYPE 0x41505031U
#define BOREDOS_APP_METADATA_MAGIC 0x414d4431U
#define BOREDOS_APP_METADATA_VERSION 1U

#define BOREDOS_APP_METADATA_MAX_APP_NAME 64
#define BOREDOS_APP_METADATA_MAX_DESCRIPTION 192
#define BOREDOS_APP_METADATA_MAX_IMAGES 4
#define BOREDOS_APP_METADATA_MAX_IMAGE_PATH 160

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t image_count;
    uint16_t reserved;
    char app_name[BOREDOS_APP_METADATA_MAX_APP_NAME];
    char description[BOREDOS_APP_METADATA_MAX_DESCRIPTION];
    char images[BOREDOS_APP_METADATA_MAX_IMAGES][BOREDOS_APP_METADATA_MAX_IMAGE_PATH];
} boredos_app_metadata_t;

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint64_t entry_point;
    uint64_t exec_entry;
    uint64_t phdr_vaddr;
    uint64_t phdr_num;
    uint64_t interp_base;
    size_t   load_size;
    bool     has_interp;
} elf_load_result_t;
struct process;
bool elf_load(const char *path, uint64_t user_pml4, struct process *proc, elf_load_result_t *out_result);

#endif
