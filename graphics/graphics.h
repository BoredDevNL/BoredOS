// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>
#include <stdbool.h>
#include "limine.h"

// Dirty rectangle structure
typedef struct {
    int x, y, w, h;
    bool active;
} DirtyRect;

void graphics_init(struct limine_framebuffer *fb);
void put_pixel(int x, int y, uint32_t color);
uint32_t graphics_get_pixel(int x, int y);
void draw_rect(int x, int y, int w, int h, uint32_t color);
 
void draw_char_bitmap(int x, int y, char c, uint32_t color);
void draw_string(int x, int y, const char *s, uint32_t color);
 
void graphics_copy_screenbuffer(uint32_t *dest);


 

// Get screen dimensions
int get_screen_width(void);
int get_screen_height(void);
uint64_t graphics_get_fb_addr(void);
int graphics_get_fb_bpp(void);
uint64_t graphics_get_fb_pitch(void);
void graphics_update_resolution(int width, int height, int bpp, void* fb_addr, int color_mode);
void graphics_present_framebuffer(void);

// Framebuffer info structure (for userspace and VFS)
typedef struct {
    void *address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint16_t bpp;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} framebuffer_info_t;

struct limine_framebuffer* graphics_get_fb_info(void);
framebuffer_info_t graphics_get_fb_params(void);
framebuffer_info_t graphics_get_fb_backing_params(void);

// Dirty rectangle management
void graphics_mark_dirty(int x, int y, int w, int h);
void graphics_mark_screen_dirty(void);
DirtyRect graphics_get_dirty_rect(void);
void graphics_clear_dirty(void);
void graphics_clear_dirty_no_lock(void);

// Double buffering
void graphics_flip_buffer(void);
void graphics_clear_back_buffer(uint32_t color);
void graphics_scroll_back_buffer(int lines);


#endif
