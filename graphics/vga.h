// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stdbool.h>

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID             0
#define VBE_DISPI_INDEX_XRES           1
#define VBE_DISPI_INDEX_YRES           2
#define VBE_DISPI_INDEX_BPP            3
#define VBE_DISPI_INDEX_ENABLE         4
#define VBE_DISPI_INDEX_BANK           5
#define VBE_DISPI_INDEX_VIRT_WIDTH     6
#define VBE_DISPI_INDEX_VIRT_HEIGHT    7
#define VBE_DISPI_INDEX_X_OFFSET       8
#define VBE_DISPI_INDEX_Y_OFFSET       9

#define VBE_DISPI_DISABLED             0x00
#define VBE_DISPI_ENABLED              0x01
#define VBE_DISPI_LFB_ENABLED          0x40

#define COLOR_MODE_NORMAL 0
#define COLOR_MODE_GRAYSCALE 1
#define COLOR_MODE_MONOCHROME 2

bool vga_set_mode(uint16_t width, uint16_t height, uint16_t bpp, void **out_framebuffer);
void vga_set_palette_grayscale(void);
void vga_set_palette_standard(void);

#endif
