// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "limine.h"
#include "graphics.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "syscall.h"
#include "process.h"
#include "ps2.h"
#include "tty.h"
#include "pty.h"

#include "io.h"
#include "fat32.h"
#include "tar.h"
#include "vfs.h"
#include "kconsole.h"
#include "kutils.h"
#include "memory_manager.h"
#include "platform.h"
#include "smp.h"
#include "work_queue.h"
#include "lapic.h"
#include "panic.h"
#include "sysfs.h"
#include "procfs.h"
#include "disk.h"
#include "bootfs.h"
#include "kernel_subsystem.h"
#include "module_manager.h"
#include "bootfs_state.h"
#include "keymap.h"
#include "keyboard.h"
#include "acpi.h"
#include "ac97.h"

extern void sysfs_init_subsystems(void);

// --- Limine Requests ---
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 1
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_bootloader_info_request bootloader_info_request = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_file_request kernel_file_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
volatile struct limine_rsdp_request acpi_rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests_start")))
static volatile struct limine_request *const requests_start_marker[] = {
    (struct limine_request *)&framebuffer_request,
    (struct limine_request *)&memmap_request,
    (struct limine_request *)&module_request,
    (struct limine_request *)&smp_request,
    (struct limine_request *)&bootloader_info_request,
    (struct limine_request *)&kernel_file_request,
    (struct limine_request *)&acpi_rsdp_request,
    NULL
};

__attribute__((used, section(".requests_end")))
static volatile struct limine_request *const requests_end_marker[] = {
    NULL
};

static void hcf(void) {
    asm("cli");
    for (;;) {
        asm("hlt");
    }
}

static void init_serial() {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static spinlock_t serial_lock = SPINLOCK_INIT;

void serial_write(const char *str) {
    uint64_t flags = spinlock_acquire_irqsave(&serial_lock);
    const char *p = str;
    while (*p) {
        char c = *p++;
        while ((inb(0x3F8 + 5) & 0x20) == 0);
        outb(0x3F8, c);
    }
    kconsole_write(str);
    spinlock_release_irqrestore(&serial_lock, flags);
}

void serial_write_num_locked(uint32_t n) {
    if (n >= 10) serial_write_num_locked(n / 10);
    char c = '0' + (n % 10);
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, c);
    kconsole_putc(c);
}

void serial_write_num(uint32_t n) {
    uint64_t flags = spinlock_acquire_irqsave(&serial_lock);
    serial_write_num_locked(n);
    spinlock_release_irqrestore(&serial_lock, flags);
}

void serial_write_hex_locked(uint64_t n) {
    char *hex = "0123456789ABCDEF";
    if (n >= 16) serial_write_hex_locked(n / 16);
    char c = hex[n % 16];
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, c);
    kconsole_putc(c);
}

void serial_write_hex(uint64_t n) {
    uint64_t flags = spinlock_acquire_irqsave(&serial_lock);
    serial_write_hex_locked(n);
    spinlock_release_irqrestore(&serial_lock, flags);
}

void log_ok(const char *msg) {
    serial_write("[  ");
    kconsole_set_color(0xFF00FF00); 
    serial_write("OK");
    kconsole_set_color(0xFFFFFFFF); 
    serial_write("  ] ");
    serial_write(msg);
    serial_write("\n");
}

void log_fail(const char *msg) {
    serial_write("[ ");
    kconsole_set_color(0xFFFF0000); 
    serial_write("FAIL");
    kconsole_set_color(0xFFFFFFFF); 
    serial_write(" ] ");
    serial_write(msg);
    serial_write("\n");
}

static void print_verbose_boot_banner(void) {
    kconsole_set_color(0xFF473ba3);
    serial_write("       @@@@\n");
    serial_write("     @@@@@@@\n");
    serial_write("      @@@@@@\n");
    serial_write("      @@@@@@@\n");
    serial_write("       @@@@@@@      @@@@@@\n");
    serial_write("        @@@@@@   @@@@@@@@@@@@\n");
    serial_write("         @@@@@@ @@@@@@@@@@@@@@a\n");
    serial_write("         @@@@@@@@@@@X  @@@@@@@@w\n");
    serial_write("          @@@@@@@@       @@@@@@@\n");
    serial_write("           @@@@@@M        @@@@@@\n");
    serial_write("           @@@@@@@        @@@@@@\n");
    serial_write("            @@@@@@@     @@@@@@@@\n");
    serial_write("             @@@@@@@@@@@@@@@@@@\n");
    serial_write("             i@@@@@@@@@@@@@@@\n");
    serial_write("              @@@@@@@\n");
    serial_write(" @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    serial_write(" @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    serial_write(" @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    kconsole_set_color(0xFFFFFFFF);
    serial_write("\n");
}


// Kernel Entry Point

static void fat32_mkdir_recursive(const char *path) {
    char temp[256];
    int i = 0;
    
    // Skip initial slash
    if (path[0] == '/') {
        temp[0] = '/';
        i = 1;
    }
    
    while (path[i] && i < 255) {
        temp[i] = path[i];
        if (path[i] == '/') {
            temp[i] = '\0';
            fat32_mkdir(temp);
            temp[i] = '/';
        }
        i++;
    }
    if (i > 0 && temp[i-1] != '/') {
        temp[i] = '\0';
        fat32_mkdir(temp);
    }
}

static bool cmdline_has_flag(const char *cmdline, const char *flag) {
    if (!cmdline || !flag || !flag[0]) return false;
    int flag_len = (int)strlen(flag);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        int len = (int)(p - start);
        if (len == flag_len && strncmp(start, flag, (size_t)flag_len) == 0) return true;
    }
    return false;
}

static bool cmdline_read_value(const char *cmdline, const char *key, char *out, int out_len) {
    if (!cmdline || !key || !out || out_len <= 1) return false;
    int key_len = (int)strlen(key);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (strncmp(p, key, (size_t)key_len) == 0) {
            const char *val = p + key_len;
            int i = 0;
            while (*val && *val != ' ' && i < out_len - 1) {
                out[i++] = *val++;
            }
            out[i] = '\0';
            return i > 0;
        }
        while (*p && *p != ' ') p++;
    }
    return false;
}

static void boot_parse_cmdline(const char *cmdline, uint32_t media_type) {
    g_bootfs_state.boot_flags = 0;
    g_bootfs_state.root_device[0] = '\0';

    char root_arg[32];
    if (cmdline_read_value(cmdline, "root=", root_arg, (int)sizeof(root_arg))) {
        const char *dev = root_arg;
        if (dev[0] == '/' && dev[1] == 'd' && dev[2] == 'e' && dev[3] == 'v' && dev[4] == '/') {
            dev += 5;
        }
        int i = 0;
        while (dev[i] && i < (int)sizeof(g_bootfs_state.root_device) - 1) {
            g_bootfs_state.root_device[i] = dev[i];
            i++;
        }
        g_bootfs_state.root_device[i] = '\0';
        if (i > 0) g_bootfs_state.boot_flags |= BOOT_FLAG_ROOT_SET;
    }

    bool force_live = cmdline_has_flag(cmdline, "--live");
    bool force_disk = cmdline_has_flag(cmdline, "--disk");

    if (force_live) {
        g_bootfs_state.boot_flags |= BOOT_FLAG_LIVE | BOOT_FLAG_FORCED;
    } else if (force_disk) {
        g_bootfs_state.boot_flags |= BOOT_FLAG_DISK | BOOT_FLAG_FORCED;
    } else if (g_bootfs_state.boot_flags & BOOT_FLAG_ROOT_SET) {
        g_bootfs_state.boot_flags |= BOOT_FLAG_DISK;
    } else if (media_type == LIMINE_MEDIA_TYPE_OPTICAL || media_type == LIMINE_MEDIA_TYPE_TFTP) {
        g_bootfs_state.boot_flags |= BOOT_FLAG_LIVE;
    } else {
        g_bootfs_state.boot_flags |= BOOT_FLAG_DISK;
    }
}


void kmain(void) {
    init_serial();
    vfs_init();
    serial_write("\n");

    platform_init();
    log_ok("Platform initialized");
    
    extern uint64_t hhdm_offset;
    extern uint64_t kernel_phys_base;
    extern uint64_t kernel_virt_base;
    
    serial_write("[INIT] HHDM Offset: 0x");
    serial_write_hex(hhdm_offset);
    serial_write("\n");
    serial_write("[INIT] Kernel Phys: 0x");
    serial_write_hex(kernel_phys_base);
    serial_write("\n");
    serial_write("[INIT] Kernel Virt: 0x");
    serial_write_hex(kernel_virt_base);
    serial_write("\n");

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_write("[INIT] No framebuffer! Halting.\n");
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    graphics_init(fb);
    kconsole_init();

    gdt_init();
    log_ok("GDT initialized");

    idt_init();
    idt_register_interrupts();

    paging_init();
    log_ok("Paging ready");

    syscall_init();
    log_ok("Syscalls ready");


    // Check for verbose boot flag
    if (kernel_file_request.response != NULL && kernel_file_request.response->kernel_file != NULL) {
        const char *cmdline = kernel_file_request.response->kernel_file->cmdline;
        if (cmdline != NULL && k_strstr(cmdline, "-v") != NULL) {
            kconsole_set_active(true);
        }
    }

    log_ok("Graphics and Console ready");

    if (memmap_request.response != NULL) {
        memory_manager_init_from_memmap(memmap_request.response);
        log_ok("Memory manager ready");
        smp_init_bsp();
        log_ok("SMP BSP initialized");
    } else {
        log_fail("No usable memory for heap! Check Limine memmap.");
        hcf();
    }

    idt_load();
    log_ok("IDT ready");
    print_verbose_boot_banner();
    kconsole_set_color(0xFFFFFF55);
    serial_write("Welcome to BoredOS!\n");
    kconsole_set_color(0xFFFFFFFF);
    acpi_init();
    
    process_init();

    fat32_init();
    log_ok("FAT32 ready");

    disk_manager_init();
    disk_manager_scan();
    
    // Initialize AC97 sound card
    ac97_init();

    sysfs_init_subsystems();
    vfs_mount("/sys", "sysfs", "sysfs", sysfs_get_ops(), NULL);
    vfs_mount("/proc", "procfs", "procfs", procfs_get_ops(), NULL);
    
    bootfs_init();
    
    if (bootloader_info_request.response != NULL) {
        if (bootloader_info_request.response->name) {
            strcpy(g_bootfs_state.bootloader_name, bootloader_info_request.response->name);
        }
        if (bootloader_info_request.response->version) {
            strcpy(g_bootfs_state.bootloader_version, bootloader_info_request.response->version);
        }
    }
    
    if (kernel_file_request.response != NULL && kernel_file_request.response->kernel_file != NULL) {
        g_bootfs_state.kernel_size = kernel_file_request.response->kernel_file->size;
        serial_write("[INIT] Kernel size from bootloader: ");
        serial_write_hex(g_bootfs_state.kernel_size);
        serial_write(" bytes\n");
    }

    if (kernel_file_request.response != NULL && kernel_file_request.response->kernel_file != NULL) {
        const char *cmdline = kernel_file_request.response->kernel_file->cmdline;
        uint32_t media_type = kernel_file_request.response->kernel_file->media_type;
        boot_parse_cmdline(cmdline, media_type);
    } else {
        boot_parse_cmdline(NULL, LIMINE_MEDIA_TYPE_GENERIC);
    }

    if (g_bootfs_state.boot_flags & BOOT_FLAG_DISK) {
        void *vol = NULL;
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
            vfs_mount_t *m = vfs_get_mount(i);
            if (m && strcmp(m->device, g_bootfs_state.root_device) == 0) {
                vol = m->fs_private;
                break;
            }
        }
        if (vol) {
            vfs_umount("/");
            vfs_mount("/", g_bootfs_state.root_device, "fat32", fat32_get_realfs_ops(), vol);
            fat32_set_root_volume(vol);
            serial_write("[INIT] Switched root to /dev/");
            serial_write(g_bootfs_state.root_device);
            serial_write("\n");
            
            extern void bootfs_mount_boot_partition(void);
            bootfs_mount_boot_partition();
        } else {
            serial_write("[INIT] Warning: Root device volume not found! Running from ramfs.\n");
        }
    }
    
    extern uint32_t kernel_ticks;
    g_bootfs_state.boot_time_ms = kernel_ticks;


    if (module_request.response != NULL) {
        g_bootfs_state.num_modules = module_request.response->module_count;
        
        serial_write("[INIT] Scanning modules for bootfs state...\n");
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];
            const char *path = mod->path;
            
            if (fs_starts_with(path, "boot():")) path += 7;
            else if (fs_starts_with(path, "boot:///")) path += 8;
            
            int path_len = 0;
            while (path[path_len]) path_len++;
            
            serial_write("[INIT] Module: ");
            serial_write(path);
            serial_write(" (");
            serial_write_hex(mod->size);
            serial_write(" bytes)\n");
            
            bool is_tar = (path_len >= 4 && 
                           path[path_len-4] == '.' && path[path_len-3] == 't' && 
                           path[path_len-2] == 'a' && path[path_len-1] == 'r');
            bool is_lz4 = (path_len >= 8 && 
                           path[path_len-8] == '.' && path[path_len-7] == 't' && 
                           path[path_len-6] == 'a' && path[path_len-5] == 'r' && 
                           path[path_len-4] == '.' && path[path_len-3] == 'l' && 
                           path[path_len-2] == 'z' && path[path_len-1] == '4');

            if (is_tar || is_lz4) {
                g_bootfs_state.initrd_size = mod->size;
                g_bootfs_state.initrd_ptr = mod->address;
                serial_write("[INIT] -> Initrd detected\n");
            }
        }
    }
    
    vfs_mount("/boot", "bootfs", "bootfs", bootfs_get_ops(), NULL);

    if (module_request.response == NULL) {
        log_fail("Limine module response NULL");
    } else if (!(g_bootfs_state.boot_flags & BOOT_FLAG_DISK)) {
        log_ok("Limine modules loaded");
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];

            const char *clean_path = mod->path;
            if (fs_starts_with(clean_path, "boot():")) clean_path += 7;
            else if (fs_starts_with(clean_path, "boot:///")) clean_path += 8;
            
            int len = 0;
            while(clean_path[len]) len++;
            
            bool is_tar = (len >= 4 && 
                           clean_path[len-4] == '.' && clean_path[len-3] == 't' && 
                           clean_path[len-2] == 'a' && clean_path[len-1] == 'r');
            bool is_lz4 = (len >= 8 && 
                           clean_path[len-8] == '.' && clean_path[len-7] == 't' && 
                           clean_path[len-6] == 'a' && clean_path[len-5] == 'r' && 
                           clean_path[len-4] == '.' && clean_path[len-3] == 'l' && 
                           clean_path[len-2] == 'z' && clean_path[len-1] == '4');

            if (is_tar || is_lz4) {
                serial_write("[INIT] Parsing initrd: ");
                serial_write(clean_path);
                serial_write("\n");
                
                if (is_lz4) {
                    uint8_t *src = (uint8_t *)mod->address;
                    uint8_t flg = src[4];
                    uint64_t uncomp_size = 0;
                    if (flg & 0x08) {
                        uncomp_size = src[6] | (src[7] << 8) | (src[8] << 16) | (src[9] << 24) |
                                      ((uint64_t)src[10] << 32) | ((uint64_t)src[11] << 40) |
                                      ((uint64_t)src[12] << 48) | ((uint64_t)src[13] << 56);
                    }
                    if (uncomp_size == 0) {
                        uncomp_size = 128 * 1024 * 1024; 
                    }
                    
                    serial_write("[INIT] Decompressing LZ4 initrd (uncompressed size: ");
                    serial_write_hex(uncomp_size);
                    serial_write(" bytes)...\n");
                    
                    uint8_t *decomp_buf = (uint8_t *)kmalloc(uncomp_size);
                    if (!decomp_buf) {
                        serial_write("[INIT] ERROR: Failed to allocate decompression buffer!\n");
                        hcf();
                    }
                    
                    extern int lz4_decompress_frame(const uint8_t *src, int src_len, uint8_t *dst, int dst_len);
                    int decomp_size = lz4_decompress_frame(mod->address, mod->size, decomp_buf, uncomp_size);
                    if (decomp_size < 0) {
                        serial_write("[INIT] ERROR: LZ4 decompression failed!\n");
                        hcf();
                    }
                    
                    serial_write("[INIT] Decompression successful! Parsing TAR...\n");
                    
                    uint8_t *shrunk_buf = (uint8_t *)krealloc(decomp_buf, decomp_size);
                    if (shrunk_buf) {
                        decomp_buf = shrunk_buf;
                    }
                    
                    tar_parse(decomp_buf, decomp_size);
                    
                    g_bootfs_state.initrd_ptr = decomp_buf;
                    g_bootfs_state.initrd_size = decomp_size;
                } else {
                    tar_parse(mod->address, mod->size);
                    g_bootfs_state.initrd_ptr = mod->address;
                    g_bootfs_state.initrd_size = mod->size;
                }
            } else {
                char dir_path[256];
                int last_slash = -1;
                for (int j = 0; clean_path[j]; j++) {
                    if (clean_path[j] == '/') last_slash = j;
                }
                if (last_slash > 0) {
                    for (int j = 0; j < last_slash; j++) dir_path[j] = clean_path[j];
                    dir_path[last_slash] = '\0';
                    fat32_mkdir_recursive(dir_path);
                }
                
                FAT32_FileHandle *fh = fat32_open(clean_path, "w");
                if (fh && fh->valid) {
                    fat32_write(fh, mod->address, mod->size);
                    fat32_close(fh);
                }
            }
            module_manager_register(clean_path, (uint64_t)mod->address, mod->size);
        }
    }
    
    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    serial_write("[INIT] Stack Alignment: 0x");
    serial_write_hex(current_rsp);
    serial_write("\n");
    ps2_init();
    asm("sti");  // Enable interrupts 
    keymap_init();
    lapic_init();

    if (smp_request.response != NULL) {
        uint32_t online = smp_init(smp_request.response);
        log_ok("SMP initialized");
    } else {
        serial_write("[INIT] No SMP response from bootloader\n");
        smp_init(NULL);
    }


    extern void bootfs_refresh_from_disk(void);
    bootfs_refresh_from_disk();

    tty_init();
    pty_init();
    kconsole_set_active(false);

    // Spawn shells for all 10 TTYs
    for (int i = 0; i < TTY_COUNT; i++) {
        char args[32];
        itoa(i + 1, args);
        process_create_elf("/bin/bsh.elf", args, true, i);
    }

    asm volatile("sti");

    // Main blitter loop
    while(1) {
        tty_blit_active();
        k_sleep(16); 
    }

}
