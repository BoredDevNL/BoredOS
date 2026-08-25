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
#include "slab.h"
#include "pmm.h"
#include "pmm_test.h"
#include "slab.h"
#include "slab_test.h"
#include "platform.h"
#include "smp.h"
#include "work_queue.h"
#include "lapic.h"
#include "panic.h"
#include "sysfs.h"
#include "procfs.h"
#include "disk.h"
#include "tmpfs.h"
#include "pagecache.h"
#include "kernel_subsystem.h"
#include "module_manager.h"
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
    .flags = LIMINE_SMP_X2APIC
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

void serial_write_mac(const char *label, const uint8_t *mac) {
    serial_write(label);
    for (int i = 0; i < 6; i++) {
        char buf[4];
        itoa_hex(mac[i], buf);
        serial_write(buf);
        if (i < 5) serial_write(":");
    }
    serial_write("\n");
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


static bool cmdline_has_flag(const char *cmdline, const char *flag) {
    if (!cmdline || !flag || !flag[0]) return false;
    size_t flag_len = strlen(flag);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = p - start;
        if (len == flag_len && strncmp(start, flag, flag_len) == 0) return true;
    }
    return false;
}

static bool cmdline_read_value(const char *cmdline, const char *key, char *out, size_t out_len) {
    if (!cmdline || !key || !out || out_len <= 1) return false;
    size_t key_len = strlen(key);
    const char *p = cmdline;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (strncmp(p, key, key_len) == 0) {
            const char *val = p + key_len;
            size_t i = 0;
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

#define BOOT_FLAG_LIVE          0x01
#define BOOT_FLAG_DISK          0x02
#define BOOT_FLAG_ROOT_SET      0x08

static uint8_t g_boot_flags = 0;
static char g_boot_root_device[32] = {0};

static void boot_parse_cmdline(const char *cmdline, uint32_t media_type) {
    g_boot_flags = 0;
    g_boot_root_device[0] = '\0';

    char root_arg[32];
    if (cmdline_read_value(cmdline, "root=", root_arg, (int)sizeof(root_arg))) {
        const char *dev = root_arg;
        if (dev[0] == '/' && dev[1] == 'd' && dev[2] == 'e' && dev[3] == 'v' && dev[4] == '/') {
            dev += 5;
        }
        int i = 0;
        while (dev[i] && i < (int)sizeof(g_boot_root_device) - 1) {
            g_boot_root_device[i] = dev[i];
            i++;
        }
        g_boot_root_device[i] = '\0';
        if (i > 0) g_boot_flags |= BOOT_FLAG_ROOT_SET;
    }

    if (g_boot_flags & BOOT_FLAG_ROOT_SET) {
        g_boot_flags |= BOOT_FLAG_DISK;
    } else if (media_type == LIMINE_MEDIA_TYPE_OPTICAL || media_type == LIMINE_MEDIA_TYPE_TFTP) {
        g_boot_flags |= BOOT_FLAG_LIVE;
    } else {
        g_boot_flags |= BOOT_FLAG_DISK;
    }
}


static void vfs_mkdir_recursive(const char *path) {
    char temp[256];
    int len = 0;
    while (path[len] && len < 255) {
        temp[len] = path[len];
        if (temp[len] == '/' && len > 0) {
            temp[len] = '\0';
            if (!vfs_exists(temp)) {
                vfs_mkdir(temp);
            }
            temp[len] = '/';
        }
        len++;
    }
    temp[len] = '\0';
    if (!vfs_exists(temp)) {
        vfs_mkdir(temp);
    }
}

static void init_early(void) {
    platform_init();
    init_serial();
    vfs_init();
    serial_write("\n");
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
}

static void init_graphics(void) {
    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_write("[INIT] No framebuffer! Halting.\n");
        hcf();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    graphics_init(fb);
    kconsole_init();
}

static void init_cpu_state(void) {
    gdt_init();
    log_ok("GDT initialized");

    idt_init();
    idt_register_interrupts();

    syscall_init();
    log_ok("Syscalls ready");
}

static void init_verbose_console(void) {
    // Check for verbose boot flag
    if (kernel_file_request.response != NULL && kernel_file_request.response->kernel_file != NULL) {
        const char *cmdline = kernel_file_request.response->kernel_file->cmdline;
        if (cmdline != NULL && k_strstr(cmdline, "-v") != NULL) {
            kconsole_set_active(true);
        }
    }

    log_ok("Graphics and Console ready");
}

static void init_memory(void) {
    if (memmap_request.response != NULL) {
        extern void pmm_init(const pmm_boot_map_t *boot_map);
        extern bool pmm_run_tests(void);
        extern uint64_t hhdm_offset;

        pmm_mem_region_t pmm_regions[memmap_request.response->entry_count];
        for (uint64_t i = 0; i < memmap_request.response->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap_request.response->entries[i];
            pmm_regions[i].base = entry->base;
            pmm_regions[i].length = entry->length;
            pmm_regions[i].type = (entry->type == LIMINE_MEMMAP_USABLE) ? PMM_REGION_USABLE : PMM_REGION_RESERVED;
        }
        pmm_boot_map_t boot_map = {
            .regions = pmm_regions,
            .region_count = memmap_request.response->entry_count,
            .direct_map_base = hhdm_offset,
        };
        pmm_init(&boot_map);

        if (pmm_run_tests()) {
            log_ok("PMM unit tests passed");
        } else {
            log_fail("PMM unit tests failed");
        }

        slab_init();
        if (slab_run_tests()) {
            log_ok("SLAB Allocator unit tests passed");
        } else {
            log_fail("SLAB Allocator unit tests failed");
        }

        extern void mmu_init(void);
        extern bool mmu_run_tests(void);
        mmu_init();
        if (mmu_run_tests()) {
            log_ok("MMU Hardware Driver unit tests passed");
        } else {
            log_fail("MMU Hardware Driver unit tests failed");
        }

        extern bool vma_run_tests(void);
        if (vma_run_tests()) {
            log_ok("VMA Augmented RB-Tree unit tests passed");
        } else {
            log_fail("VMA Augmented RB-Tree unit tests failed");
        }

        extern void vmm_init(void);
        extern bool vmm_run_tests(void);
        vmm_init();
        if (vmm_run_tests()) {
            log_ok("VMM Demand Paging unit tests passed");
        } else {
            log_fail("VMM Demand Paging unit tests failed");
        }

        extern void pagecache_init(void);
        extern bool pagecache_run_tests(void);
        pagecache_init();
        if (pagecache_run_tests()) {
            log_ok("Page Cache unit tests passed");
        } else {
            log_fail("Page Cache unit tests failed");
        }

        smp_init_bsp();
        log_ok("SMP BSP initialized");
    } else {
        log_fail("No usable memory for heap! Check Limine memmap.");
        hcf();
    }
}

static void init_banner_and_acpi(void) {
    idt_load();
    log_ok("IDT ready");
    print_verbose_boot_banner();
    kconsole_set_color(0xFFFFFF55);
    serial_write("Welcome to BoredOS!\n");
    kconsole_set_color(0xFFFFFFFF);
    acpi_init();
}

static void init_subsystems(void) {
    process_init();

    extern void futex_init(void);
    futex_init();

    fat32_init();
    log_ok("FAT32 ready");

    disk_manager_init();
    disk_manager_scan();
    
    // Initialize AC97 sound card
    ac97_init();

    sysfs_init_subsystems();
    vfs_mount("/sys", "sysfs", "sysfs", sysfs_get_ops(), NULL);
    vfs_mount("/proc", "procfs", "procfs", procfs_get_ops(), NULL);
}

static void init_rootfs(void) {
    if (kernel_file_request.response != NULL && kernel_file_request.response->kernel_file != NULL) {
        const char *cmdline = kernel_file_request.response->kernel_file->cmdline;
        uint32_t media_type = kernel_file_request.response->kernel_file->media_type;
        boot_parse_cmdline(cmdline, media_type);
    } else {
        boot_parse_cmdline(NULL, LIMINE_MEDIA_TYPE_GENERIC);
    }

    tmpfs_init();
    vfs_mount("/", "tmpfs", "tmpfs", tmpfs_get_ops(), NULL);

    if (g_boot_flags & BOOT_FLAG_DISK) {
        Disk *d = NULL;
        if (g_boot_root_device[0] != '\0') {
            d = disk_get_by_name(g_boot_root_device);
        }
        if (!d) {
            int total_disks = disk_get_count();
            for (int i = 0; i < total_disks; i++) {
                Disk *cand = disk_get_by_index(i);
                if (cand && cand->is_partition && !cand->is_esp) {
                    d = cand;
                    break;
                }
            }
        }

        void *vol = NULL;
        const char *fs_type = NULL;
        vfs_fs_ops_t *ops = NULL;

        if (d) {
            extern void *ext4fs_mount_volume(Disk *d);
            extern vfs_fs_ops_t *ext4fs_get_ops(void);
            extern void *fat32_mount_volume(void *disk_ptr);
            extern struct vfs_fs_ops *fat32_get_realfs_ops(void);

            if (!d->is_fat32) {
                vol = ext4fs_mount_volume(d);
                if (vol) {
                    fs_type = "ext4";
                    ops = ext4fs_get_ops();
                }
            }
            if (!vol) {
                vol = fat32_mount_volume(d);
                if (vol) {
                    fs_type = "fat32";
                    ops = (vfs_fs_ops_t *)fat32_get_realfs_ops();
                }
            }
        }

        if (d && vol && fs_type && ops) {
            vfs_umount("/");
            vfs_mount("/", d->devname, fs_type, ops, vol);
            if (strcmp(fs_type, "fat32") == 0) {
                fat32_set_root_volume(vol);
            }
            serial_write("[INIT] Switched root to /dev/");
            serial_write(d->devname);
            serial_write(" (");
            serial_write(fs_type);
            serial_write(")\n");

            // Auto-mount ESP to /boot if available
            int total_disks = disk_get_count();
            for (int i = 0; i < total_disks; i++) {
                Disk *esp = disk_get_by_index(i);
                if (esp && esp->is_partition && esp->is_esp && esp->is_fat32) {
                    void *esp_vol = fat32_mount_volume(esp);
                    if (esp_vol) {
                        vfs_mount("/boot", esp->devname, "fat32", (vfs_fs_ops_t *)fat32_get_realfs_ops(), esp_vol);
                        serial_write("[INIT] Mounted ESP at /boot\n");
                    }
                    break;
                }
            }

            // Ensure runtime directories and /tmp tmpfs exist on the new root
            vfs_mkdir("/tmp");
            vfs_mount("/tmp", "tmpfs", "tmpfs", tmpfs_get_ops(), NULL);
            vfs_mkdir("/var");
            vfs_mkdir("/var/run");
            vfs_mkdir("/dev");
        } else {
            serial_write("[INIT] Warning: Root device volume not found! Running from tmpfs.\n");
        }
    }

    extern void flusher_init(void);
    flusher_init();
}


static void init_modules(void) {
    if (module_request.response == NULL) {
        log_fail("Limine module response NULL");
    } else if (!(g_boot_flags & BOOT_FLAG_DISK)) {
        log_ok("Limine modules loaded");
        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];

            const char *clean_path = mod->path;
            if (str_starts_with(clean_path, "boot():")) clean_path += 7;
            else if (str_starts_with(clean_path, "boot:///")) clean_path += 8;
            
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
                    
                    tar_parse(decomp_buf, decomp_size);
                    kfree(decomp_buf);
                } else {
                    tar_parse(mod->address, mod->size);
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
                    vfs_mkdir_recursive(dir_path);
                }
                
                vfs_file_t *fh = vfs_open(clean_path, "w");
                if (fh) {
                    vfs_write(fh, mod->address, (int)mod->size);
                    vfs_close(fh);
                }
            }
            module_manager_register(clean_path, (uint64_t)mod->address, mod->size);
        }
    }
}

static void init_input(void) {
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
}

static void init_tty(void) {

    extern void hostname_init(void);
    hostname_init();

    tty_init();
    pty_init();
    kconsole_set_active(false);

    /* Spawn the zombie reaper daemon first so it registers via SYS_SET_REAPER
     * before any user processes can become orphans. 
     * ps: what's with people coming up with terms like orphans and stuff?
     * says something about the devs that came up with it, hm?*/
    process_create_elf("/bin/job_applications.elf", "", false, -1);

    // Spawn shells for all 10 TTYs
    for (int i = 0; i < TTY_COUNT; i++) {
        char args[32];
        itoa(i + 1, args);
        process_create_elf("/bin/bsh.elf", args, true, i);
    }
}

void kmain(void) {
    init_early();
    init_graphics();
    init_cpu_state();
    init_verbose_console();
    init_memory();
    init_banner_and_acpi();
    init_subsystems();
    init_rootfs();
    init_modules();
    init_input();
    init_tty();

    asm volatile("sti");

    // Main blitter loop
    while(1) {
        tty_blit_active();
        k_sleep(16); 
    }

}
