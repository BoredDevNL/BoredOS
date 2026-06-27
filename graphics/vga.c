// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "vga.h"
#include "io.h"
#include "pci.h"
#include "platform.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);

static void vga_write_register(uint16_t index, uint16_t data) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, data);
}

bool vga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, void **out_framebuffer) {
    // 1. Locate the BGA PCI device to get the physical framebuffer address
    pci_device_t bga_dev;
    // Standard VGA compatible controller is Class 0x03, Subclass 0x00
    if (!pci_find_device_by_class(0x03, 0x00, &bga_dev)) {
        serial_write("[VGA] Error: VGA compatible controller not found via PCI!\n");
        return false;
    }

    // Read BAR0 (offset 0x10)
    uint32_t bar0 = pci_read_config(bga_dev.bus, bga_dev.device, bga_dev.function, 0x10);
    // physical address is bar0 with the lower 4 bits masked out
    uint32_t phys_base = bar0 & 0xFFFFFFF0;
    
    if (phys_base == 0) {
        serial_write("[VGA] Error: Invalid BAR0 physical base.\n");
        return false;
    }

    serial_write("[VGA] Found VGA Controller. LFB Phys Base: ");
    serial_write_num(phys_base);
    serial_write("\n");

    // Map physical to virtual using p2v
    void *vram_ptr = (void *)p2v(phys_base);

    // 2. Disable BGA extensions first
    vga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    // 3. Set resolution and BPP
    vga_write_register(VBE_DISPI_INDEX_XRES, width);
    vga_write_register(VBE_DISPI_INDEX_YRES, height);
    vga_write_register(VBE_DISPI_INDEX_BPP, bpp);

    // 4. Re-enable BGA and Linear Framebuffer
    vga_write_register(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    if (out_framebuffer) {
        *out_framebuffer = vram_ptr;
    }

    return true;
}

void vga_set_palette_grayscale(void) {
    outb(0x03C8, 0); // Palette index
    for (int i = 0; i < 256; i++) {
        // VGA palette uses 6 bits per channel (0-63)
        uint8_t val = i >> 2;
        outb(0x03C9, val); // R
        outb(0x03C9, val); // G
        outb(0x03C9, val); // B
    }
}

void vga_set_palette_standard(void) {
    outb(0x03C8, 0); // Palette index
    for (int i = 0; i < 256; i++) {
        // Standard 3:3:2 RGB mapping to 6-bit per channel
        uint8_t r = (i >> 5) & 0x7;
        uint8_t g = (i >> 2) & 0x7;
        uint8_t b = i & 0x3;
        
        outb(0x03C9, (r * 63) / 7);
        outb(0x03C9, (g * 63) / 7);
        outb(0x03C9, (b * 63) / 3);
    }
}
