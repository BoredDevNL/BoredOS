// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "kernel_subsystem.h"
#include "smp.h"
#include "pci.h"
#include "memory_manager.h"
#include "module_manager.h"
#include "io.h"
#include "kutils.h"
#include "graphics.h"
#include "platform.h"
#include "disk.h"

// --- Helper: itoa ---
static void sys_itoa(int n, char *s) {
    itoa(n, s);
}

// --- Graphics Implementation ---
static int read_gfx_drm(char *buf, int size, int offset) {
    char out[512];
    memset(out, 0, 512);
    strcpy(out, "Driver: Simple Framebuffer\n");
    strcpy(out + strlen(out), "Resolution: ");
    char s[32]; itoa(get_screen_width(), s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "x");
    itoa(get_screen_height(), s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "\nDepth: ");
    itoa(graphics_get_fb_bpp(), s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), " bpp\nAddress: 0x");
    itoa_hex(graphics_get_fb_addr(), s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "\n");

    int len = (int)strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- Memory Tracking Implementation ---
static int read_mem_tracking(char *buf, int size, int offset) {
    MemStats stats = memory_get_stats();
    char out[1024];
    memset(out, 0, 1024);
    
    strcpy(out, "--- Kernel Heap Tracking ---\n");
    strcpy(out + strlen(out), "Allocated Blocks: ");
    char s[32]; itoa(stats.allocated_blocks, s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "\nFragmentation: ");
    itoa(stats.fragmentation_percent, s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "%\n");

    int len = (int)strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- Module Implementation ---
static int read_sys_modules(char *buf, int size, int offset) {
    int count = module_manager_get_count();
    char out[2048] = "Loaded Modules:\n";
    
    for (int i = 0; i < count; i++) {
        kernel_module_t *mod = module_manager_get_index(i);
        strcpy(out + strlen(out), "  - ");
        strcpy(out + strlen(out), mod->name);
        strcpy(out + strlen(out), " (");
        char sz_s[16]; itoa(mod->size / 1024, sz_s);
        strcpy(out + strlen(out), sz_s);
        strcpy(out + strlen(out), " KB)\n");
    }

    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- PCI Bus Implementation ---
static int read_pci_bus(char *buf, int size, int offset) {
    pci_device_t devices[64];
    int count = pci_enumerate_devices(devices, 64);
    
    char out[4096];
    memset(out, 0, 4096);
    strcpy(out, "PCI Bus Devices:\n");
    for (int i = 0; i < count; i++) {
        char line[128];
        strcpy(line, " [");
        char b_s[8]; itoa(devices[i].bus, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), ":");
        itoa(devices[i].device, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), ":");
        itoa(devices[i].function, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), "] Vendor:");
        itoa_hex(devices[i].vendor_id, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), " Device:");
        itoa_hex(devices[i].device_id, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), " Class:");
        itoa_hex(devices[i].class_code, b_s);
        strcpy(line + strlen(line), b_s);
        strcpy(line + strlen(line), "\n");
        
        if (strlen(out) + strlen(line) < 4095) {
            strcpy(out + strlen(out), line);
        }
    }

    int len = (int)strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- CPU System Implementation ---
static int read_cpu_info(char *buf, int size, int offset) {
    char *out = (char*)kmalloc(16384);
    if (!out) return 0;
    out[0] = 0;
    
    char vendor[16];
    char model[64];
    char flags[1024];
    cpu_info_t info;
    
    platform_get_cpu_vendor(vendor);
    platform_get_cpu_model(model);
    platform_get_cpu_info(&info);
    platform_get_cpu_flags(flags);
    
    uint32_t cpu_count = smp_cpu_count();
    
    for (uint32_t i = 0; i < cpu_count; i++) {
        char c_s[32];
        
        strcpy(out + strlen(out), "processor\t: ");
        itoa(i, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "vendor_id\t: ");
        strcpy(out + strlen(out), vendor);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "cpu family\t: ");
        itoa(info.family, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "model\t\t: ");
        itoa(info.model, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "model name\t: ");
        strcpy(out + strlen(out), model);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "stepping\t: ");
        itoa(info.stepping, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "microcode\t: 0x");
        char hex[16];
        int temp = info.microcode;
        int hex_pos = 0;
        for (int j = 7; j >= 0; j--) {
            int digit = (temp >> (j * 4)) & 0xF;
            hex[hex_pos++] = digit < 10 ? '0' + digit : 'a' + (digit - 10);
        }
        hex[hex_pos] = '\0';
        strcpy(out + strlen(out), hex);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "cache size\t: ");
        itoa(info.cache_size, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), " KB\n");
        
        strcpy(out + strlen(out), "physical id\t: 0\n");
        strcpy(out + strlen(out), "siblings\t: ");
        itoa(cpu_count, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "core id\t\t: ");
        itoa(i, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "cpu cores\t: ");
        itoa(cpu_count, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "apicid\t\t: ");
        itoa(i, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "initial apicid\t: ");
        itoa(i, c_s);
        strcpy(out + strlen(out), c_s);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "fpu\t\t: yes\n");
        strcpy(out + strlen(out), "fpu_exception\t: yes\n");
        
        strcpy(out + strlen(out), "cpuid level\t: 13\n");
        
        strcpy(out + strlen(out), "wp\t\t: yes\n");
        
        strcpy(out + strlen(out), "flags\t\t: ");
        strcpy(out + strlen(out), flags);
        strcpy(out + strlen(out), "\n");
        
        strcpy(out + strlen(out), "bugs\t\t: \n");
        strcpy(out + strlen(out), "bogomips\t: 4800.00\n");
        
        if (i < cpu_count - 1) {
            strcpy(out + strlen(out), "\n");
        }
    }
    
    int len = (int)strlen(out);
    if (offset >= len) { kfree(out); return 0; }
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    kfree(out);
    return to_copy;
}

// --- Devices Implementation ---
static int read_sys_devices(char *buf, int size, int offset) {
    char out[2048];
    memset(out, 0, 2048);
    
    extern int disk_get_count(void);
    extern Disk* disk_get_by_index(int index);
    
    int dcount = disk_get_count();
    strcpy(out, "Block Devices:\n");
    for (int i = 0; i < dcount; i++) {
        Disk *d = disk_get_by_index(i);
        if (d && !d->is_partition) {
            strcpy(out + strlen(out), "  ");
            strcpy(out + strlen(out), d->devname);
            strcpy(out + strlen(out), " - ");
            strcpy(out + strlen(out), d->label);
            strcpy(out + strlen(out), "\n");
        }
    }
    
    strcpy(out + strlen(out), "\nCharacter Devices:\n");
    strcpy(out + strlen(out), "  console - System console\n");
    strcpy(out + strlen(out), "  tty - Terminal devices\n");
    strcpy(out + strlen(out), "  psmouse - Mouse input\n");
    strcpy(out + strlen(out), "  keyboard - Keyboard input\n");
    strcpy(out + strlen(out), "  framebuffer - Framebuffer device\n");
    
    int len = (int)strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- Class Implementation ---
static int read_sys_class(char *buf, int size, int offset) {
    char out[1024];
    memset(out, 0, 1024);
    
    strcpy(out, "Classes:\n");
    strcpy(out + strlen(out), "  block - Block device class\n");
    strcpy(out + strlen(out), "  input - Input device class\n");
    strcpy(out + strlen(out), "  tty - TTY device class\n");
    strcpy(out + strlen(out), "  sound - Sound device class\n");
    strcpy(out + strlen(out), "  video - Video device class\n");
    strcpy(out + strlen(out), "  net - Network device class\n");
    
    int len = (int)strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

// --- GPIO Implementation ---
static int read_gpio_debug(char *buf, int size, int offset) {
    uint8_t p64 = inb(0x64);
    char out[64] = "Port 0x64 Status: ";
    char s[16]; itoa(p64, s);
    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "\n");
    
    int len = strlen(out);
    if (offset >= len) return 0;
    int to_copy = len - offset;
    if (to_copy > size) to_copy = size;
    memcpy(buf, out + offset, to_copy);
    return to_copy;
}

void sysfs_init_subsystems(void) {
    kernel_subsystem_t *kernel, *devices, *bus, *class, *debug, *mem_debug;
    
    subsystem_register("kernel", &kernel);
    subsystem_register("devices", &devices);
    subsystem_register("bus", &bus);
    subsystem_register("class", &class);
    subsystem_register("kernel/debug", &debug);
    
    // Devices info
    subsystem_add_file(devices, "list", read_sys_devices, NULL);
    
    // Class info
    subsystem_add_file(class, "list", read_sys_class, NULL);
    
    // CPU info
    subsystem_add_file(kernel, "cpuinfo", read_cpu_info, NULL);
    
    // Bus info
    kernel_subsystem_t *pci_bus;
    subsystem_register("bus/pci", &pci_bus);
    subsystem_add_file(pci_bus, "devices", read_pci_bus, NULL);

    // Module info
    kernel_subsystem_t *modules_sub;
    subsystem_register("module", &modules_sub);
    subsystem_add_file(modules_sub, "loaded", read_sys_modules, NULL);

    // Memory Tracking
    subsystem_register("kernel/debug/memory", &mem_debug);
    subsystem_add_file(mem_debug, "tracking", read_mem_tracking, NULL);
    
    // Graphics DRM
    kernel_subsystem_t *gfx_debug;
    subsystem_register("kernel/debug/graphics", &gfx_debug);
    subsystem_add_file(gfx_debug, "drm", read_gfx_drm, NULL);

    // GPIO
    subsystem_add_file(debug, "gpio", read_gpio_debug, NULL);

    // Network Interface Class
    extern void sysfs_net_init(void);
    sysfs_net_init();
}
