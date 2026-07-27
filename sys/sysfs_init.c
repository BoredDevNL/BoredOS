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
#include <limits.h>

// --- Graphics Implementation ---
static int read_gfx_drm(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    char out[512];
    memset(out, 0, 512);

    strcpy(out, "Driver: Simple Framebuffer\n");
    strcpy(out + strlen(out), "Resolution: ");

    char s[32];

    itoa(get_screen_width(), s);
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

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

// --- Memory Tracking Implementation ---
static int read_mem_tracking(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    MemStats stats = memory_get_stats();

    char out[1024];
    memset(out, 0, 1024);

    strcpy(out, "--- Kernel Heap Tracking ---\n");
    strcpy(out + strlen(out), "Allocated Blocks: ");

    char s[32];

    itoa(stats.allocated_blocks, s);
    strcpy(out + strlen(out), s);

    strcpy(out + strlen(out), "\nFragmentation: ");

    itoa(stats.fragmentation_percent, s);
    strcpy(out + strlen(out), s);

    strcpy(out + strlen(out), "%\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

// --- Module Implementation ---
static int read_sys_modules(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    int count = module_manager_get_count();

    char out[2048] = "Loaded Modules:\n";

    if (count < 0)
        return -1;

    for (int i = 0; i < count; i++) {
        kernel_module_t *mod = module_manager_get_index(i);

        if (!mod)
            continue;

        strcpy(out + strlen(out), "  - ");
        strcpy(out + strlen(out), mod->name);
        strcpy(out + strlen(out), " (");

        char sz_s[16];

        itoa(mod->size / 1024, sz_s);

        strcpy(out + strlen(out), sz_s);
        strcpy(out + strlen(out), " KB)\n");
    }

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

// --- PCI Bus Implementation ---
static int read_pci_bus(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0)
        return -1;

    pci_device_t devices[64];
    int count = pci_enumerate_devices(devices, 64);

    if (count < 0)
        return -1;

    char out[4096];
    memset(out, 0, 4096);

    strcpy(out, "PCI Bus Devices:\n");

    for (int i = 0; i < count; i++) {
        char line[128];
        memset(line, 0, sizeof(line));

        strcpy(line, " [");

        char b_s[16];

        itoa(devices[i].bus, b_s);
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

        if (strlen(out) + strlen(line) < sizeof(out) - 1) {
            strcpy(out + strlen(out), line);
        }
    }

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

static int read_ticks_info(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    extern uint32_t get_ticks(void);

    uint32_t ticks = get_ticks();

    char out[32];

    itoa(ticks, out);
    strcpy(out + strlen(out), "\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

static int read_mem_info(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    MemStats stats = memory_get_stats();

    char out[128];
    char temp[32];

    out[0] = 0;

    utoa(stats.total_memory, temp);
    strcpy(out, temp);
    strcpy(out + strlen(out), "\n");

    utoa(stats.used_memory, temp);
    strcpy(out + strlen(out), temp);
    strcpy(out + strlen(out), "\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

static int read_keyboard_layout(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0) return -1;

    extern int keymap_get_current(void);

    int layout = keymap_get_current();

    char out[16];

    itoa(layout, out);
    strcpy(out + strlen(out), "\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

static int write_keyboard_layout(const char *buf, size_t size, size_t offset) {
    (void)offset;

    if (!buf && size > 0)
        return -1;

    int val = 0;

    for (size_t i = 0; i < size && buf[i] >= '0' && buf[i] <= '9'; i++) {
        if (val > (INT_MAX / 10))
            return -1;

        val = val * 10 + (buf[i] - '0');
    }

    extern void keymap_set_current(int id);

    keymap_set_current(val);

    if (size > INT_MAX)
        return -1;

    return (int)size;
}

// --- CPU System Implementation ---
static int read_cpu_info(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0)
        return -1;

    char *out = (char*)kmalloc(16384);
    if (!out)
        return -1;

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

        itoa_hex32(info.microcode, hex);
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

        if (i < cpu_count - 1)
            strcpy(out + strlen(out), "\n");
    }

    size_t len = strlen(out);

    if (offset >= len) {
        kfree_null(out);
        return 0;
    }

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX) {
        kfree_null(out);
        return -1;
    }

    memcpy(buf, out + offset, to_copy);

    kfree_null(out);

    return (int)to_copy;
}

// --- Devices Implementation ---
static int read_sys_devices(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0)
        return -1;

    char out[2048];
    memset(out, 0, 2048);

    extern int disk_get_count(void);
    extern Disk* disk_get_by_index(int index);

    int dcount = disk_get_count();

    if (dcount < 0)
        return -1;

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

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

// --- Class Implementation ---
static int read_sys_class(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0)
        return -1;

    char out[1024];
    memset(out, 0, 1024);

    strcpy(out, "Classes:\n");
    strcpy(out + strlen(out), "  block - Block device class\n");
    strcpy(out + strlen(out), "  input - Input device class\n");
    strcpy(out + strlen(out), "  tty - TTY device class\n");
    strcpy(out + strlen(out), "  sound - Sound device class\n");
    strcpy(out + strlen(out), "  video - Video device class\n");
    strcpy(out + strlen(out), "  net - Network device class\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}


// --- GPIO Implementation ---
static int read_gpio_debug(char *buf, size_t size, size_t offset) {
    if (!buf && size > 0)
        return -1;

    uint8_t p64 = inb(0x64);

    char out[64] = "Port 0x64 Status: ";

    char s[16];

    itoa(p64, s);

    strcpy(out + strlen(out), s);
    strcpy(out + strlen(out), "\n");

    size_t len = strlen(out);

    if (offset >= len)
        return 0;

    size_t to_copy = len - offset;

    if (to_copy > size)
        to_copy = size;

    if (to_copy > INT_MAX)
        return -1;

    memcpy(buf, out + offset, to_copy);

    return (int)to_copy;
}

void sysfs_init_subsystems(void) {
    kernel_subsystem_t *kernel, *devices, *bus, *class, *debug, *mem_debug;

    subsystem_register("kernel", &kernel);
    subsystem_register("devices", &devices);
    subsystem_register("bus", &bus);
    subsystem_register("class", &class);
    subsystem_register("kernel/debug", &debug);

    if (!kernel || !devices || !bus || !class || !debug)
        return;

    // Devices info
    subsystem_add_file(devices, "list", read_sys_devices, NULL);

    // Class info
    subsystem_add_file(class, "list", read_sys_class, NULL);

    // CPU info
    subsystem_add_file(kernel, "cpuinfo", read_cpu_info, NULL);
    subsystem_add_file(kernel, "ticks", read_ticks_info, NULL);
    subsystem_add_file(kernel, "meminfo", read_mem_info, NULL);
    subsystem_add_file(kernel, "keyboard_layout", read_keyboard_layout, write_keyboard_layout);

    // Bus info
    kernel_subsystem_t *pci_bus;

    subsystem_register("bus/pci", &pci_bus);

    if (pci_bus)
        subsystem_add_file(pci_bus, "devices", read_pci_bus, NULL);

    // Module info
    kernel_subsystem_t *modules_sub;

    subsystem_register("module", &modules_sub);

    if (modules_sub)
        subsystem_add_file(modules_sub, "loaded", read_sys_modules, NULL);

    // Memory Tracking
    subsystem_register("kernel/debug/memory", &mem_debug);

    if (mem_debug)
        subsystem_add_file(mem_debug, "tracking", read_mem_tracking, NULL);

    // Graphics DRM
    kernel_subsystem_t *gfx_debug;

    subsystem_register("kernel/debug/graphics", &gfx_debug);

    if (gfx_debug)
        subsystem_add_file(gfx_debug, "drm", read_gfx_drm, NULL);

    // GPIO
    subsystem_add_file(debug, "gpio", read_gpio_debug, NULL);

    // Network Interface Class
    extern void sysfs_net_init(void);
    sysfs_net_init();
}
