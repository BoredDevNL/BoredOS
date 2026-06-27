// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include <stddef.h>
#include "graphics.h"
#include "font.h"
#include "io.h"
#include "memory_manager.h"
#include "kutils.h"
#include "spinlock.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static struct limine_framebuffer *g_fb = NULL;
static spinlock_t graphics_lock = SPINLOCK_INIT;
static DirtyRect g_dirty = {0, 0, 0, 0, false};


extern void serial_write(const char *str);

static int g_color_mode = 0;
#define MAX_FB_WIDTH 2048
#define MAX_FB_HEIGHT 2048
static uint32_t g_back_buffer[MAX_FB_WIDTH * MAX_FB_HEIGHT] __attribute__((aligned(4096)));

extern uint32_t smp_this_cpu_id(void);

void graphics_init(struct limine_framebuffer *fb) {
    g_fb = fb;
    g_dirty.active = false;
    // Initialize back buffer to black
    for (int i = 0; i < MAX_FB_WIDTH * MAX_FB_HEIGHT; i++) {
        g_back_buffer[i] = 0;
    }
}


void graphics_update_resolution(int width, int height, int bpp, void* fb_addr, int color_mode) {
    if (!g_fb) return;
    
    uint64_t rflags;
    asm volatile("pushfq; pop %0; cli" : "=r"(rflags));
    
    g_fb->width = width;
    g_fb->height = height;
    g_fb->bpp = bpp;
    g_fb->pitch = width * (bpp / 8);
    g_fb->address = fb_addr;
    g_color_mode = color_mode;
    
    // Clear back buffer
    for (int i = 0; i < MAX_FB_WIDTH * MAX_FB_HEIGHT; i++) {
        g_back_buffer[i] = 0;
    }
    
    // Clear dirty rect
    g_dirty.active = false;
    
    asm volatile("push %0; popfq" : : "r"(rflags));
}


int get_screen_width(void) {
    return g_fb ? g_fb->width : 0;
}

int get_screen_height(void) {
    return g_fb ? g_fb->height : 0;
}

uint64_t graphics_get_fb_addr(void) {
    return g_fb ? (uint64_t)g_fb->address : 0;
}

int graphics_get_fb_bpp(void) {
    return g_fb ? g_fb->bpp : 0;
}

uint64_t graphics_get_fb_pitch(void) {
    return g_fb ? g_fb->pitch : 0;
}

struct limine_framebuffer* graphics_get_fb_info(void) {
    return g_fb;
}

framebuffer_info_t graphics_get_fb_params(void) {
    framebuffer_info_t info = {0};
    if (g_fb) {
        info.address = g_fb->address;
        info.width = g_fb->width;
        info.height = g_fb->height;
        info.pitch = g_fb->pitch;
        info.bpp = g_fb->bpp;
        info.red_mask_size = g_fb->red_mask_size;
        info.red_mask_shift = g_fb->red_mask_shift;
        info.green_mask_size = g_fb->green_mask_size;
        info.green_mask_shift = g_fb->green_mask_shift;
        info.blue_mask_size = g_fb->blue_mask_size;
        info.blue_mask_shift = g_fb->blue_mask_shift;
    }
    return info;
}

framebuffer_info_t graphics_get_fb_backing_params(void) {
    framebuffer_info_t info = {0};
    if (g_fb) {
        info.address = g_back_buffer;
        info.width = g_fb->width;
        info.height = g_fb->height;
        info.pitch = g_fb->pitch;
        info.bpp = g_fb->bpp;
        info.red_mask_size = g_fb->red_mask_size;
        info.red_mask_shift = g_fb->red_mask_shift;
        info.green_mask_size = g_fb->green_mask_size;
        info.green_mask_shift = g_fb->green_mask_shift;
        info.blue_mask_size = g_fb->blue_mask_size;
        info.blue_mask_shift = g_fb->blue_mask_shift;
    }
    return info;
}

// faltten the structure, cache the edges, calculate the right and bottom edges
// calculate a new bounding box with some of my clever branchless math
// and perform exactly 1 Write to memory
static void merge_dirty_rect(int x, int y, int w, int h) {
    if (!g_dirty.active) {
        g_dirty.x = x;
        g_dirty.y = y;
        g_dirty.w = w;
        g_dirty.h = h;
        g_dirty.active = true;
        return; 
    }

    int gx = g_dirty.x;
    int gy = g_dirty.y;
    
    int gx2 = gx + g_dirty.w;
    int gy2 = gy + g_dirty.h;
    int nx2 = x + w;
    int ny2 = y + h;

    int new_x = MIN(gx, x);
    int new_y = MIN(gy, y);
    int new_x2 = MAX(gx2, nx2);
    int new_y2 = MAX(gy2, ny2);

    g_dirty.x = new_x;
    g_dirty.y = new_y;
    g_dirty.w = new_x2 - new_x;
    g_dirty.h = new_y2 - new_y;
}

void graphics_mark_dirty(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return;
    }

    int x2 = x + w;
    int y2 = y + h;

    //  Cache screen boundaries because then we can avoid multiple calls
    int screen_w = get_screen_width();
    int screen_h = get_screen_height();

    int cx1 = MAX(0, x);
    int cy1 = MAX(0, y);
    int cx2 = MIN(screen_w, x2);
    int cy2 = MIN(screen_h, y2);

    int cw = cx2 - cx1;
    int ch = cy2 - cy1;

    if (cw <= 0 || ch <= 0) {
        return;
    }

    uint64_t flags = spinlock_acquire_irqsave(&graphics_lock);
    merge_dirty_rect(cx1, cy1, cw, ch);
    spinlock_release_irqrestore(&graphics_lock, flags);
}

void graphics_mark_screen_dirty(void) {
    uint64_t flags = spinlock_acquire_irqsave(&graphics_lock);
    g_dirty.x = 0;
    g_dirty.y = 0;
    g_dirty.w = get_screen_width();
    g_dirty.h = get_screen_height();
    g_dirty.active = true;
    spinlock_release_irqrestore(&graphics_lock, flags);
}

DirtyRect graphics_get_dirty_rect(void) {
    return g_dirty;
}

void graphics_clear_dirty(void) {
    uint64_t flags = spinlock_acquire_irqsave(&graphics_lock);
    g_dirty.active = false;
    spinlock_release_irqrestore(&graphics_lock, flags);
}

void graphics_clear_dirty_no_lock(void) {
    g_dirty.active = false;
}


void put_pixel(int x, int y, uint32_t color) {
    if (!g_fb) return;
    if (x < 0 || x >= (int)g_fb->width || y < 0 || y >= (int)g_fb->height) return;
    uint32_t pixel_offset = y * g_fb->width + x;
    g_back_buffer[pixel_offset] = color;
}

uint32_t graphics_get_pixel(int x, int y) {
    if (!g_fb) return 0;
    if (x < 0 || x >= (int)g_fb->width || y < 0 || y >= (int)g_fb->height) return 0;
    return g_back_buffer[y * g_fb->width + x];
}

void draw_rect(int x, int y, int w, int h, uint32_t color) {
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;

    if (!g_fb) return;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)g_fb->width) x2 = g_fb->width;
    if (y2 > (int)g_fb->height) y2 = g_fb->height;

    if (x1 >= x2 || y1 >= y2) return;

    for (int i = y1; i < y2; i++) {
        uint32_t *row = &g_back_buffer[i * g_fb->width + x1];
        int len = x2 - x1;
        for (int j = 0; j < len; j++) {
            row[j] = color;
        }
    }
}


void draw_char_bitmap(int x, int y, char c, uint32_t color) {
    unsigned char uc = (unsigned char)c;
    if (uc > 127) return;
    const uint8_t *glyph = font8x8_basic[uc];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if ((glyph[row] >> (7 - col)) & 1) {
                put_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_string(int x, int y, const char *s, uint32_t color) {
    if (!s) return;
    int cur_x = x;
    int cur_y = y;
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            cur_x = x;
            cur_y += 10;
            continue;
        }
        if (c == '\t') {
            cur_x += 8 * 4;
            continue;
        }
        draw_char_bitmap(cur_x, cur_y, c, color);
        cur_x += 8;
    }
}

// Double buffering functions
void graphics_clear_back_buffer(uint32_t color) {
    if (!g_fb) return;
    uint32_t *buf = g_back_buffer;
    for (int i = 0; i < (int)g_fb->width * (int)g_fb->height; i++) {
        *buf++ = color;
    }
}

void graphics_flip_buffer(void) {
    if (!g_fb) return;
    extern bool g_in_panic;
    extern bool tty_get_blit_enabled(void);
    if (!g_in_panic && !tty_get_blit_enabled()) return;

    uint64_t flags = spinlock_acquire_irqsave(&graphics_lock);
    if (!g_dirty.active) {
        spinlock_release_irqrestore(&graphics_lock, flags);
        return;
    }

    int x = g_dirty.x;
    int y = g_dirty.y;
    int w = g_dirty.w;
    int h = g_dirty.h;
    
    // Clear dirty state
    g_dirty.active = false;
    spinlock_release_irqrestore(&graphics_lock, flags);

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_fb->width) w = g_fb->width - x;
    if (y + h > (int)g_fb->height) h = g_fb->height - y;

    if (w <= 0 || h <= 0) return;

    for (int i = 0; i < h; i++) {
        int curr_y = y + i;
        uint32_t *src_row = &g_back_buffer[curr_y * g_fb->width + x];
        
        if (g_fb->bpp == 32) {
            uint32_t *dst_row = (uint32_t *)((uint8_t *)g_fb->address + curr_y * g_fb->pitch) + x;
            memcpy(dst_row, src_row, w * 4);
        } else if (g_fb->bpp == 16) {
            uint16_t *dst_row = (uint16_t *)((uint8_t *)g_fb->address + curr_y * g_fb->pitch) + x;
            for (int j = 0; j < w; j++) {
                uint32_t c = src_row[j];
                uint16_t r = ((c >> 16) & 0xFF) >> 3;
                uint16_t g = ((c >> 8)  & 0xFF) >> 2;
                uint16_t b = (c         & 0xFF) >> 3;
                dst_row[j] = (r << 11) | (g << 5) | b;
            }
        } else if (g_fb->bpp == 8) {
            uint8_t *dst_row = (uint8_t *)((uint8_t *)g_fb->address + curr_y * g_fb->pitch) + x;
            if (g_color_mode == 1) { // Grayscale
                for (int j = 0; j < w; j++) {
                    uint32_t c = src_row[j];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    dst_row[j] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                }
            } else if (g_color_mode == 2) { // Monochrome
                static const uint8_t bayer2[2][2] = {
                    {  0, 128 },
                    {192,  64 }
                };
                for (int j = 0; j < w; j++) {
                    uint32_t c = src_row[j];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    
                    int gray = (r * 77 + g * 150 + b * 29) >> 8;
                    
                    gray = gray * 2;
                    if (gray > 255) gray = 255;
                    
                    int sx = x + j;
                    uint8_t threshold = bayer2[curr_y & 1][sx & 1];
                    
                    dst_row[j] = (gray > threshold) ? 255 : 0;
                }
            } else { // 256 Colors (Standard)
                for (int j = 0; j < w; j++) {
                    uint32_t c = src_row[j];
                    uint8_t r = ((c >> 16) & 0xFF) >> 5;
                    uint8_t g = ((c >> 8) & 0xFF) >> 5;
                    uint8_t b = (c & 0xFF) >> 6;
                    dst_row[j] = (r << 5) | (g << 2) | b;
                }
            }
        }
    }
}

void graphics_copy_screenbuffer(uint32_t *dest) {
    if (!g_fb || !dest) return;
    
    uint64_t rflags = spinlock_acquire_irqsave(&graphics_lock);

    int sw = (int)g_fb->width;
    int sh = (int)g_fb->height;
    
    // Copy from the composition back buffer, applying color mode transformations if necessary
    for (int y = 0; y < sh; y++) {
        uint32_t *src_row = &g_back_buffer[y * sw];
        for (int x = 0; x < sw; x++) {
            uint32_t px = src_row[x];
            
            if (g_color_mode == 1) { // 8-bit Grayscale
                uint8_t r = (px >> 16) & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = px & 0xFF;
                uint8_t gray = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                dest[y * sw + x] = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
            } else if (g_color_mode == 2) { // 1-bit Monochrome (Dithered)
                static const uint8_t bayer2[2][2] = {
                    {  0, 128 },
                    {192,  64 }
                };
                uint8_t r = (px >> 16) & 0xFF;
                uint8_t g = (px >> 8) & 0xFF;
                uint8_t b = px & 0xFF;
                int gray = (r * 77 + g * 150 + b * 29) >> 8;
                
                // Boost contrast (matches graphics_flip_buffer logic)
                gray = gray * 2;
                if (gray > 255) gray = 255;
                
                uint8_t threshold = bayer2[y & 1][x & 1];
                dest[y * sw + x] = (gray > threshold) ? 0xFFFFFFFF : 0xFF000000;
            } else {
                // 32-bit (Standard)
                dest[y * sw + x] = px;
            }
        }
    }
    
    spinlock_release_irqrestore(&graphics_lock, rflags);
}

void graphics_present_framebuffer(void) {
    if (!g_fb) return;
    uint64_t flags = spinlock_acquire_irqsave(&graphics_lock);
    DirtyRect dr = g_dirty;
    if (g_dirty.active) {
        g_dirty.active = false;
    }

    int sx = 0, sy = 0, sw = (int)g_fb->width, sh = (int)g_fb->height;
    if (dr.active) {
        sx = dr.x;
        sy = dr.y;
        sw = dr.w;
        sh = dr.h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > (int)g_fb->width) sw = g_fb->width - sx;
        if (sy + sh > (int)g_fb->height) sh = g_fb->height - sy;
        if (sw <= 0 || sh <= 0) {
            spinlock_release_irqrestore(&graphics_lock, flags);
            return;
        }
    }

    int fb_w = (int)g_fb->width;
    int fb_h = (int)g_fb->height;
    int fb_bpp = (int)g_fb->bpp;
    int fb_pitch = (int)g_fb->pitch;
    void *fb_addr = g_fb->address;

    if (fb_bpp == 32 && g_color_mode == 0) {
        for (int row = sy; row < sy + sh; row++) {
            uint32_t *src = &g_back_buffer[row * fb_w + sx];
            uint8_t *dst_row = (uint8_t *)fb_addr + (uint64_t)row * fb_pitch + sx * 4;
            memcpy(dst_row, src, (size_t)sw * 4);
        }
        spinlock_release_irqrestore(&graphics_lock, flags);
        return;
    }

    for (int row = sy; row < sy + sh; row++) {
        uint32_t *src_row = &g_back_buffer[row * fb_w + sx];

        if (fb_bpp == 16) {
            uint16_t *dst_row = (uint16_t *)((uint8_t *)fb_addr + (uint64_t)row * fb_pitch) + sx;
            for (int x = 0; x < sw; x++) {
                uint32_t c = src_row[x];
                uint16_t r = ((c >> 16) & 0xFF) >> 3;
                uint16_t g = ((c >> 8)  & 0xFF) >> 2;
                uint16_t b = (c         & 0xFF) >> 3;
                dst_row[x] = (r << 11) | (g << 5) | b;
            }
        } else if (fb_bpp == 8) {
            uint8_t *dst_row = (uint8_t *)((uint8_t *)fb_addr + (uint64_t)row * fb_pitch) + sx;
            if (g_color_mode == 1) {
                for (int x = 0; x < sw; x++) {
                    uint32_t c = src_row[x];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    dst_row[x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                }
            } else if (g_color_mode == 2) {
                static const uint8_t bayer2[2][2] = {
                    {  0, 128 },
                    {192,  64 }
                };
                for (int x = 0; x < sw; x++) {
                    uint32_t c = src_row[x];
                    uint8_t r = (c >> 16) & 0xFF;
                    uint8_t g = (c >> 8) & 0xFF;
                    uint8_t b = c & 0xFF;
                    int gray = (r * 77 + g * 150 + b * 29) >> 8;
                    gray = gray * 2;
                    if (gray > 255) gray = 255;
                    int sxp = sx + x;
                    uint8_t threshold = bayer2[row & 1][sxp & 1];
                    dst_row[x] = (gray > threshold) ? 255 : 0;
                }
            } else {
                for (int x = 0; x < sw; x++) {
                    uint32_t c = src_row[x];
                    uint8_t r = ((c >> 16) & 0xFF) >> 5;
                    uint8_t g = ((c >> 8) & 0xFF) >> 5;
                    uint8_t b = (c & 0xFF) >> 6;
                    dst_row[x] = (r << 5) | (g << 2) | b;
                }
            }
        } else {
            uint32_t *dst_row32 = (uint32_t *)((uint8_t *)fb_addr + (uint64_t)row * fb_pitch) + sx;
            for (int x = 0; x < sw; x++) dst_row32[x] = src_row[x];
        }
    }
    spinlock_release_irqrestore(&graphics_lock, flags);
}

void graphics_copy_buffer(uint32_t *src) {
    if (!g_fb || !src) return;
    extern bool g_in_panic;
    extern bool tty_get_blit_enabled(void);
    if (!g_in_panic && !tty_get_blit_enabled()) return;
    uint8_t *dst = (uint8_t *)g_fb->address;
    int width = g_fb->width;
    int height = g_fb->height;
    int pitch = g_fb->pitch;
    
    for (int y = 0; y < height; y++) {
        memcpy(dst + (y * pitch), src + (y * width), width * 4);
    }
}


void graphics_scroll_back_buffer(int lines) {
    if (!g_fb || lines <= 0 || lines >= (int)g_fb->height) return;
    uint64_t rflags = spinlock_acquire_irqsave(&graphics_lock);

    int sw = (int)g_fb->width;
    int sh = (int)g_fb->height;
    
    for (int y = 0; y < sh - lines; y++) {
        uint32_t *dst = &g_back_buffer[y * sw];
        uint32_t *src = &g_back_buffer[(y + lines) * sw];
        for (int x = 0; x < sw; x++) dst[x] = src[x];
    }
    
    for (int y = sh - lines; y < sh; y++) {
        uint32_t *dst = &g_back_buffer[y * sw];
        for (int x = 0; x < sw; x++) dst[x] = 0;
    }
    
    spinlock_release_irqrestore(&graphics_lock, rflags);
}
