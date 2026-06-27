// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "tty.h"
#include "pty.h"
#include "spinlock.h"
#include "wait_queue.h"
#include "font.h"
#include "memory_manager.h"
#include "graphics.h"
#include "kutils.h"
#include <stdbool.h>
#include <stdint.h>

static tty_t g_ttys[TTY_COUNT];
static int g_active_tty = 0;
static spinlock_t g_tty_global_lock = SPINLOCK_INIT;
static uint32_t *g_active_tty_vfb = NULL;

#include "kutils.h"

static void tty_queue_init(tty_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    wait_queue_init(&q->wait_queue);
    memset(q->buffer, 0, TTY_IN_QUEUE_SIZE);
}

static void tty_queue_push(tty_queue_t *q, uint8_t val) {
    uint32_t next = (q->head + 1) % TTY_IN_QUEUE_SIZE;
    if (next != q->tail) {
        q->buffer[q->head] = val;
        q->head = next;
        wait_queue_wake_all(&q->wait_queue);
    }
}

static int tty_queue_pop(tty_queue_t *q, uint8_t *buf, size_t len) {
    size_t count = 0;
    while (q->head != q->tail && count < len) {
        buf[count++] = q->buffer[q->tail];
        q->tail = (q->tail + 1) % TTY_IN_QUEUE_SIZE;
    }
    return (int)count;
}

void tty_init(void) {
    int w = get_screen_width();
    int h = get_screen_height();
    size_t vfb_size = w * h * 4;

    graphics_clear_back_buffer(0xFF000000);
    graphics_mark_screen_dirty();
    graphics_flip_buffer();

    g_active_tty_vfb = (uint32_t *)graphics_get_fb_backing_params().address;
    for (size_t j = 0; j < vfb_size / 4; j++) g_active_tty_vfb[j] = 0xFF000000;
    int cols = w / 8;
    int rows = h / 8;

    for (int i = 0; i < TTY_COUNT; i++) {
        g_ttys[i].id = i;
        g_ttys[i].used = true;
        g_ttys[i].width = w;
        g_ttys[i].height = h;
        g_ttys[i].grid = (tty_cell_t *)kmalloc(cols * rows * sizeof(tty_cell_t));
        g_ttys[i].dirty = true;
        g_ttys[i].cursor_x = 0;
        g_ttys[i].cursor_y = 0;
        g_ttys[i].cursor_visible = true;
        g_ttys[i].fg_color = 0xFFFFFFFF;
        g_ttys[i].bg_color = 0xFF000000;
        g_ttys[i].blit_enabled = true;
        g_ttys[i].fg_pid = -1;
        g_ttys[i].esc_state = 0;
        g_ttys[i].esc_num_params = 0;
        g_ttys[i].saved_x = 0;
        g_ttys[i].saved_y = 0;
        g_ttys[i].utf8_state = 0;
        g_ttys[i].utf8_codepoint = 0;
        g_ttys[i].lock = SPINLOCK_INIT;
        
        for (int j = 0; j < cols * rows; j++) {
            g_ttys[i].grid[j].codepoint = ' ';
            g_ttys[i].grid[j].fg = 0xFFFFFFFF;
            g_ttys[i].grid[j].bg = 0xFF000000;
        }
        tty_queue_init(&g_ttys[i].key_queue);
        tty_queue_init(&g_ttys[i].mouse_queue);
        tty_queue_init(&g_ttys[i].out_queue);
        tty_queue_init(&g_ttys[i].char_queue);
    }

    g_active_tty = 0;
}

tty_t* tty_get(int id) {
    if (id < 0 || id >= TTY_COUNT) return NULL;
    return &g_ttys[id];
}

void tty_switch(int id) {
    if (id < 0 || id >= TTY_COUNT) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_tty_global_lock);
    g_active_tty = id;
    g_ttys[id].dirty = true;
    spinlock_release_irqrestore(&g_tty_global_lock, flags);
}

int tty_get_active_id(void) {
    return g_active_tty;
}

static void tty_draw_rect(tty_t *t, int x, int y, int w, int h, uint32_t color) {
    int cols = t->width / 8;
    int rows = t->height / 8;
    int start_col = x / 8;
    int start_row = y / 8;
    int num_cols = w / 8;
    int num_rows = h / 8;
    for (int r = start_row; r < start_row + num_rows; r++) {
        if (r < 0 || r >= rows) continue;
        for (int c = start_col; c < start_col + num_cols; c++) {
            if (c < 0 || c >= cols) continue;
            int idx = r * cols + c;
            t->grid[idx].codepoint = ' ';
            t->grid[idx].fg = t->fg_color;
            t->grid[idx].bg = color;
        }
    }
    t->dirty = true;
}

static bool get_box_drawing_glyph(uint32_t codepoint, uint8_t glyph[8]) {
    memset(glyph, 0, 8);
    
    if (codepoint == 0x2500) { 
        glyph[3] = 0xFF; glyph[4] = 0xFF; 
        return true;
    }
    if (codepoint == 0x2502) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x18; 
        return true;
    }
    if (codepoint == 0x250C) {
        glyph[3] = 0x1F; glyph[4] = 0x1F; 
        glyph[5] = glyph[6] = glyph[7] = 0x18; 
        return true;
    }
    if (codepoint == 0x2510) { 
        glyph[3] = 0xF8; glyph[4] = 0xF8; 
        glyph[5] = glyph[6] = glyph[7] = 0x18; 
        return true;
    }
    if (codepoint == 0x2514) { 
        glyph[3] = 0x1F; glyph[4] = 0x1F; 
        glyph[0] = glyph[1] = glyph[2] = 0x18; 
        return true;
    }
    if (codepoint == 0x2518) { 
        glyph[3] = 0xF8; glyph[4] = 0xF8; 
        glyph[0] = glyph[1] = glyph[2] = 0x18; 
        return true;
    }
    if (codepoint == 0x251C) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x18;
        glyph[3] |= 0x0F; glyph[4] |= 0x0F; 
        return true;
    }
    if (codepoint == 0x2524) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x18; 
        glyph[3] |= 0xF0; glyph[4] |= 0xF0; 
        return true;
    }
    if (codepoint == 0x252C) { 
        glyph[3] = 0xFF; glyph[4] = 0xFF; 
        glyph[5] = glyph[6] = glyph[7] = 0x18; 
        return true;
    }
    if (codepoint == 0x2534) { 
        glyph[3] = 0xFF; glyph[4] = 0xFF; 
        glyph[0] = glyph[1] = glyph[2] = 0x18; 
        return true;
    }
    if (codepoint == 0x253C) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x18;
        glyph[3] = 0xFF; glyph[4] = 0xFF; 
        return true;
    }
    
    if (codepoint == 0x2550) {
        glyph[2] = 0xFF; glyph[5] = 0xFF;
        return true;
    }
    if (codepoint == 0x2551) {
        for (int r = 0; r < 8; r++) glyph[r] = 0x24;
        return true;
    }
    if (codepoint == 0x2554) { 
        glyph[2] = 0x3F; glyph[5] = 0x0F;
        glyph[3] = glyph[4] = 0x24;
        glyph[6] = glyph[7] = 0x24;
        return true;
    }
    if (codepoint == 0x2557) { 
        glyph[2] = 0xFC; glyph[5] = 0xF0;
        glyph[3] = glyph[4] = 0x24;
        glyph[6] = glyph[7] = 0x24;
        return true;
    }
    if (codepoint == 0x255A) {
        glyph[5] = 0x3F; glyph[2] = 0x0F;
        glyph[3] = glyph[4] = 0x24;
        glyph[0] = glyph[1] = 0x24;
        return true;
    }
    if (codepoint == 0x255D) { 
        glyph[5] = 0xFC; glyph[2] = 0xF0;
        glyph[3] = glyph[4] = 0x24;
        glyph[0] = glyph[1] = 0x24;
        return true;
    }
    if (codepoint == 0x2560) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x24;
        glyph[2] |= 0x3F; glyph[5] |= 0x3F;
        return true;
    }
    if (codepoint == 0x2563) {
        for (int r = 0; r < 8; r++) glyph[r] = 0x24;
        glyph[2] |= 0xFC; glyph[5] |= 0xFC;
        return true;
    }
    if (codepoint == 0x2566) { 
        glyph[2] = 0xFF; glyph[5] = 0xFF;
        glyph[3] = glyph[4] = 0x24;
        glyph[6] = glyph[7] = 0x24;
        return true;
    }
    if (codepoint == 0x2569) { 
        glyph[2] = 0xFF; glyph[5] = 0xFF;
        glyph[3] = glyph[4] = 0x24;
        glyph[0] = glyph[1] = 0x24;
        return true;
    }
    if (codepoint == 0x256C) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0x24;
        glyph[2] = 0xFF; glyph[5] = 0xFF;
        return true;
    }

    if (codepoint == 0x2588) { 
        for (int r = 0; r < 8; r++) glyph[r] = 0xFF;
        return true;
    }
    
    return false;
}

static void tty_render_char_to_vfb(uint32_t *dest, int width, int height, int x, int y, uint32_t codepoint, uint32_t fg, uint32_t bg) {
    if (x < 0 || x + 8 > width || y < 0 || y + 8 > height) return;
    
    uint8_t custom_glyph[8];
    const uint8_t *glyph;
    if (get_box_drawing_glyph(codepoint, custom_glyph)) {
        glyph = custom_glyph;
    } else {
        uint32_t uc = codepoint;
        if (uc > 127) uc = 0;
        glyph = font8x8_basic[uc];
    }
    
    for (int row = 0; row < 8; row++) {
        uint32_t *vfb_row = &dest[(y + row) * width + x];
        uint8_t glyph_row = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((glyph_row >> (7 - col)) & 1) {
                vfb_row[col] = fg;
            } else {
                vfb_row[col] = bg;
            }
        }
    }
}

static void tty_render_grid_to_vfb(tty_t *t, uint32_t *dest) {
    int cols = t->width / 8;
    int rows = t->height / 8;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            tty_cell_t cell = t->grid[r * cols + c];
            tty_render_char_to_vfb(dest, t->width, t->height, c * 8, r * 8, cell.codepoint, cell.fg, cell.bg);
        }
    }
}
static void tty_write_cell(tty_t *t, int col, int row, uint32_t codepoint, uint32_t fg, uint32_t bg) {
    int cols = t->width / 8;
    int rows = t->height / 8;
    if (col < 0 || col >= cols || row < 0 || row >= rows) return;
    int idx = row * cols + col;
    t->grid[idx].codepoint = codepoint;
    t->grid[idx].fg = fg;
    t->grid[idx].bg = bg;
    t->dirty = true;
}

static uint32_t g_cursor_backup[8 * 8];

static void tty_composite_cursor_vfb(tty_t *t, uint32_t *dest) {
    if (!t->cursor_visible) return;
    if (t->cursor_x < 0 || t->cursor_x + 8 > t->width) return;
    if (t->cursor_y < 0 || t->cursor_y + 8 > t->height) return;

    for (int r = 0; r < 8; r++) {
        uint32_t *row = &dest[(t->cursor_y + r) * t->width + t->cursor_x];
        for (int c = 0; c < 8; c++) {
            g_cursor_backup[r * 8 + c] = row[c];
            row[c] = (~row[c]) | 0xFF000000; 
        }
    }
}

static void tty_restore_cursor_vfb(tty_t *t, uint32_t *dest) {
    if (!t->cursor_visible) return;
    if (t->cursor_x < 0 || t->cursor_x + 8 > t->width) return;
    if (t->cursor_y < 0 || t->cursor_y + 8 > t->height) return;

    for (int r = 0; r < 8; r++) {
        uint32_t *row = &dest[(t->cursor_y + r) * t->width + t->cursor_x];
        for (int c = 0; c < 8; c++) {
            row[c] = g_cursor_backup[r * 8 + c];
        }
    }
}

static void tty_scroll(tty_t *t) {
    int cols = t->width / 8;
    int rows = t->height / 8;
    memmove(t->grid, t->grid + cols, (rows - 1) * cols * sizeof(tty_cell_t));
        for (int col = 0; col < cols; col++) {
        t->grid[(rows - 1) * cols + col].codepoint = ' ';
        t->grid[(rows - 1) * cols + col].fg = t->fg_color;
        t->grid[(rows - 1) * cols + col].bg = t->bg_color;
    }
    t->cursor_y -= 8;
    t->dirty = true;
}

void tty_write(int id, const char *data, size_t len) {
    if (pty_is_pty_id(id)) {
        pty_write_output(id, data, len);
        return;
    }
    tty_t *t = tty_get(id);
    if (!t) return;
    uint64_t flags = spinlock_acquire_irqsave(&t->lock);
    int font_w = 8;
    int font_h = 8;
    
    for (size_t i = 0; i < len; i++) {
        char raw_c = data[i];
        uint32_t c = 0;
        bool has_codepoint = false;
        unsigned char uc = (unsigned char)raw_c;
        
        if (t->utf8_state == 0) {
            if ((uc & 0x80) == 0) {
                c = uc;
                has_codepoint = true;
            } else if ((uc & 0xE0) == 0xC0) {
                t->utf8_codepoint = uc & 0x1F;
                t->utf8_state = 1;
            } else if ((uc & 0xF0) == 0xE0) {
                t->utf8_codepoint = uc & 0x0F;
                t->utf8_state = 2;
            } else if ((uc & 0xF8) == 0xF0) {
                t->utf8_codepoint = uc & 0x07;
                t->utf8_state = 3;
            }
        } else {
            if ((uc & 0xC0) == 0x80) {
                t->utf8_codepoint = (t->utf8_codepoint << 6) | (uc & 0x3F);
                t->utf8_state--;
                if (t->utf8_state == 0) {
                    c = t->utf8_codepoint;
                    has_codepoint = true;
                }
            } else {
                t->utf8_state = 0;
                if ((uc & 0x80) == 0) {
                    c = uc;
                    has_codepoint = true;
                } else if ((uc & 0xE0) == 0xC0) {
                    t->utf8_codepoint = uc & 0x1F;
                    t->utf8_state = 1;
                } else if ((uc & 0xF0) == 0xE0) {
                    t->utf8_codepoint = uc & 0x0F;
                    t->utf8_state = 2;
                } else if ((uc & 0xF8) == 0xF0) {
                    t->utf8_codepoint = uc & 0x07;
                    t->utf8_state = 3;
                }
            }
        }
        
        if (!has_codepoint) continue;
        
        if (t->esc_state == 1) { 
            if (c == '[') {
                t->esc_state = 2;
                t->esc_num_params = 0;
                for (int p = 0; p < 8; p++) t->esc_params[p] = 0;
            } else if (c == 's') { 
                t->saved_x = t->cursor_x;
                t->saved_y = t->cursor_y;
                t->esc_state = 0;
            } else if (c == 'u') { 
                t->cursor_x = t->saved_x;
                t->cursor_y = t->saved_y;
                t->esc_state = 0;
            } else {
                t->esc_state = 0;
            }
            continue;
        } else if (t->esc_state == 2) { // Saw ESC [
            if (c == '?') {
                t->esc_state = 3;  // DEC private mode
                t->esc_num_params = 0;
                for (int p = 0; p < 8; p++) t->esc_params[p] = 0;
                continue;
            }
            if (c >= '0' && c <= '9') {
                t->esc_params[t->esc_num_params] = t->esc_params[t->esc_num_params] * 10 + (c - '0');
            } else if (c == ';') {
                if (t->esc_num_params < 7) t->esc_num_params++;
            } else {
                // Final command character
                if (c == 'K') { // Erase to end of line
                    tty_draw_rect(t, t->cursor_x, t->cursor_y, t->width - t->cursor_x, font_h, t->bg_color);
                } else if (c == 'J') { // Erase in Display
                    int mode = t->esc_params[0];
                    if (mode == 2) { // Entire screen
                        tty_draw_rect(t, 0, 0, t->width, t->height, t->bg_color);
                    } else if (mode == 1) { // From beginning to cursor
                        tty_draw_rect(t, 0, 0, t->width, t->cursor_y, t->bg_color);
                        tty_draw_rect(t, 0, t->cursor_y, t->cursor_x, font_h, t->bg_color);
                    } else { // From cursor to end
                        tty_draw_rect(t, t->cursor_x, t->cursor_y, t->width - t->cursor_x, font_h, t->bg_color);
                        if (t->cursor_y + font_h < t->height) {
                            tty_draw_rect(t, 0, t->cursor_y + font_h, t->width, t->height - (t->cursor_y + font_h), t->bg_color);
                        }
                    }
                } else if (c == 'H' || c == 'f') { // Home / Position
                    int row = t->esc_params[0];
                    int col = t->esc_params[1];
                    if (row > 0) row--; // 1-indexed to 0-indexed
                    if (col > 0) col--;
                    t->cursor_x = col * font_w;
                    t->cursor_y = row * font_h;
                    // Clamp
                    if (t->cursor_x >= t->width) t->cursor_x = t->width - font_w;
                    if (t->cursor_y >= t->height) t->cursor_y = t->height - font_h;
                } else if (c == 'A') { // Up
                    int n = t->esc_params[0]; if (n == 0) n = 1;
                    t->cursor_y -= n * font_h;
                    if (t->cursor_y < 0) t->cursor_y = 0;
                } else if (c == 'B') { // Down
                    int n = t->esc_params[0]; if (n == 0) n = 1;
                    t->cursor_y += n * font_h;
                    if (t->cursor_y >= t->height) t->cursor_y = t->height - font_h;
                } else if (c == 'C') { // Forward/Right
                    int n = t->esc_params[0]; if (n == 0) n = 1;
                    t->cursor_x += n * font_w;
                    if (t->cursor_x >= t->width) t->cursor_x = t->width - font_w;
                } else if (c == 'D') { // Backward/Left
                    int n = t->esc_params[0]; if (n == 0) n = 1;
                    t->cursor_x -= n * font_w;
                    if (t->cursor_x < 0) t->cursor_x = 0;
                } else if (c == 'm') { // SGR (Color)
                    for (int j = 0; j <= t->esc_num_params; j++) {
                        int p = t->esc_params[j];
                        if (p == 0) { t->fg_color = 0xFFFFFFFF; t->bg_color = 0xFF000000; }
                        else if (p == 1) { /* Bold */ }
                        else if (p >= 30 && p <= 37) {
                            static const uint32_t colors[] = {
                                0xFF000000, 0xFFFF4444, 0xFF6A9955, 0xFFFFCC00,
                                0xFF569CD6, 0xFFC586C0, 0xFF4EC9B0, 0xFFFFFFFF
                            };
                            t->fg_color = colors[p - 30];
                        } else if (p == 38) { // Extended FG
                            if (j + 2 <= t->esc_num_params && t->esc_params[j+1] == 5) { // 256 color
                                uint8_t color_index = (uint8_t)t->esc_params[j+2];
                                // Basic 256 color to RGB
                                if (color_index < 16) {
                                     static const uint32_t colors[] = {
                                        0xFF000000, 0xFF800000, 0xFF008000, 0xFF808000,
                                        0xFF000080, 0xFF800080, 0xFF008080, 0xFFC0C0C0,
                                        0xFF808080, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
                                        0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF
                                     };
                                     t->fg_color = colors[color_index];
                                } else {
                                     t->fg_color = 0xFFCCCCCC; // Fallback
                                }
                                j += 2;
                            } else if (j + 4 <= t->esc_num_params && t->esc_params[j+1] == 2) { // TrueColor
                                t->fg_color = 0xFF000000 | (t->esc_params[j+2] << 16) | (t->esc_params[j+3] << 8) | t->esc_params[j+4];
                                j += 4;
                            }
                        } else if (p == 39) { t->fg_color = 0xFFFFFFFF; }
                        else if (p >= 40 && p <= 47) {
                            static const uint32_t colors[] = {
                                0xFF000000, 0xFFFF4444, 0xFF6A9955, 0xFFFFCC00,
                                0xFF569CD6, 0xFFC586C0, 0xFF4EC9B0, 0xFFFFFFFF
                            };
                            t->bg_color = colors[p - 40];
                        } else if (p >= 100 && p <= 107) {
                            static const uint32_t colors[] = {
                                0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
                                0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF
                            };
                            t->bg_color = colors[p - 100];
                        } else if (p == 48) { // Extended BG
                             if (j + 2 <= t->esc_num_params && t->esc_params[j+1] == 5) {
                                j += 2; // Ignore for now
                            } else if (j + 4 <= t->esc_num_params && t->esc_params[j+1] == 2) {
                                t->bg_color = 0xFF000000 | (t->esc_params[j+2] << 16) | (t->esc_params[j+3] << 8) | t->esc_params[j+4];
                                j += 4;
                            }
                        } else if (p == 49) { t->bg_color = 0xFF000000; }
                        else if (p >= 90 && p <= 97) { // Bright FG
                            static const uint32_t colors[] = {
                                0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
                                0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF
                            };
                            t->fg_color = colors[p - 90];
                        }
                    }
                } else if (c == 's') { // Save cursor
                    t->saved_x = t->cursor_x;
                    t->saved_y = t->cursor_y;
                } else if (c == 'u') { // Restore cursor
                    t->cursor_x = t->saved_x;
                    t->cursor_y = t->saved_y;
                }
                t->esc_state = 0;
            }
            continue;
        } else if (t->esc_state == 3) { // Saw ESC [ ?  (DEC private mode)
            if (c >= '0' && c <= '9') {
                t->esc_params[t->esc_num_params] = t->esc_params[t->esc_num_params] * 10 + (c - '0');
            } else if (c == ';') {
                if (t->esc_num_params < 7) t->esc_num_params++;
            } else if (c == 'h' || c == 'l') {  // Set (h) or Reset (l) mode
                // Check for cursor visibility mode (25)
                if (t->esc_params[0] == 25) {
                    t->cursor_visible = (c == 'h');  // h = show, l = hide
                }
                t->esc_state = 0;
            } else {
                t->esc_state = 0;
            }
            continue;
        }

        if (c == '\x1b') {
            t->esc_state = 1;
            continue;
        }
        if (c == '\n') {
            t->cursor_x = 0;
            t->cursor_y += font_h;
        } else if (c == '\r') {
            t->cursor_x = 0;
        } else if (c == '\t') {
            t->cursor_x = (t->cursor_x + (font_w * 4)) & ~((font_w * 4) - 1);
        } else if (c == '\b') {
            if (t->cursor_x >= font_w) {
                t->cursor_x -= font_w;
                tty_draw_rect(t, t->cursor_x, t->cursor_y, font_w, font_h, t->bg_color);
            }
        } else {
            tty_write_cell(t, t->cursor_x / 8, t->cursor_y / 8, c, t->fg_color, t->bg_color);
            t->cursor_x += font_w;
        }

        if (t->cursor_x + font_w > t->width) {
            t->cursor_x = 0;
            t->cursor_y += font_h;
        }

        if (t->cursor_y + font_h > t->height) {
            tty_scroll(t);
        }
    }
    
    spinlock_release_irqrestore(&t->lock, flags);
}

void tty_push_key(int id, uint8_t scancode) {
    tty_t *t = tty_get(id);
    if (!t) return;
    tty_queue_push(&t->key_queue, scancode);
}

void tty_push_mouse(int id, uint8_t *packet, size_t len) {
    tty_t *t = tty_get(id);
    if (!t) return;
    for (size_t i = 0; i < len; i++) {
        tty_queue_push(&t->mouse_queue, packet[i]);
    }
}

int tty_read_key(int id, uint8_t *buf, size_t len) {
    tty_t *t = tty_get(id);
    if (!t) return 0;
    return tty_queue_pop(&t->key_queue, buf, len);
}

int tty_read_mouse(int id, uint8_t *buf, size_t len) {
    tty_t *t = tty_get(id);
    if (!t) return 0;
    return tty_queue_pop(&t->mouse_queue, buf, len);
}

void tty_push_char(int id, uint8_t c) {
    tty_t *t = tty_get(id);
    if (!t) return;
    tty_queue_push(&t->char_queue, c);
}

int tty_read_input(int id, char *buf, size_t len) {
    if (pty_is_pty_id(id)) return pty_read_input(id, buf, len);
    tty_t *t = tty_get(id);
    if (!t) return 0;
    return tty_queue_pop(&t->char_queue, (uint8_t*)buf, len);
}

int tty_create(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_tty_global_lock);
    for (int i = 0; i < TTY_COUNT; i++) {
        if (!g_ttys[i].used) {
            g_ttys[i].used = true;
            spinlock_release_irqrestore(&g_tty_global_lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&g_tty_global_lock, flags);
    return -1;
}

#define KDSETMODE   0x4B3A
#define KD_TEXT     0x00
#define KD_GRAPHICS 0x01

int tty_ioctl(int id, uint64_t request, void *arg) {
    tty_t *t = tty_get(id);
    if (!t) return -1;
    
    if (request == TIOCGWINSZ) {
        struct winsize *ws = (struct winsize *)arg;
        ws->ws_row = t->height / 8;
        ws->ws_col = t->width / 8;
        ws->ws_xpixel = t->width;
        ws->ws_ypixel = t->height;
        return 0;
    } else if (request == KDSETMODE) {
        uint64_t mode = (uint64_t)arg;
        if (mode == KD_GRAPHICS) {
            t->blit_enabled = false;
        } else if (mode == KD_TEXT) {
            t->blit_enabled = true;
        }
        return 0;
    } else if (request == 0x5606) { // VT_ACTIVATE
        tty_switch(id);
        return 0;
    }
    
    return -1;
}

int tty_destroy(int id) {
    tty_t *t = tty_get(id);
    if (!t) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&g_tty_global_lock);
    t->used = false;
    spinlock_release_irqrestore(&g_tty_global_lock, flags);
    return 0;
}

void tty_write_output(int id, const char *data, size_t len) {
    if (pty_is_pty_id(id)) { pty_write_output(id, data, len); return; }
    tty_t *t = tty_get(id);
    if (!t) return;
    for (size_t i = 0; i < len; i++) {
        tty_queue_push(&t->out_queue, (uint8_t)data[i]);
    }
}

int tty_read_output(int id, char *buf, size_t len) {
    if (pty_is_pty_id(id)) return pty_read_output(id, buf, len);
    tty_t *t = tty_get(id);
    if (!t) return 0;
    return tty_queue_pop(&t->out_queue, (uint8_t*)buf, len);
}

int tty_write_input(int id, const char *buf, size_t len) {
    if (pty_is_pty_id(id)) return pty_write_input(id, buf, len);
    tty_t *t = tty_get(id);
    if (!t) return 0;
    for (size_t i = 0; i < len; i++) {
        tty_queue_push(&t->char_queue, (uint8_t)buf[i]);
    }
    return (int)len;
}

int tty_set_foreground(int id, int pid) {
    if (pty_is_pty_id(id)) return pty_set_foreground(id, pid);
    tty_t *t = tty_get(id);
    if (!t) return -1;
    t->fg_pid = pid;
    return 0;
}

int tty_get_foreground(int id) {
    if (pty_is_pty_id(id)) return pty_get_foreground(id);
    tty_t *t = tty_get(id);
    if (!t) return -1;
    return t->fg_pid;
}

void tty_set_blit_enabled_for_id(int id, bool enabled) {
    if (pty_is_pty_id(id)) return;
    tty_t *t = tty_get(id);
    if (t) t->blit_enabled = enabled;
}

void tty_set_blit_enabled(bool enabled) {
    tty_set_blit_enabled_for_id(g_active_tty, enabled);
}
bool tty_get_blit_enabled(void) {
    tty_t *t = tty_get(g_active_tty);
    if (!t) return true;
    return t->blit_enabled;
}

void tty_blit_active(void) {
    tty_t *t = tty_get(g_active_tty);
    if (!t || !t->blit_enabled || !g_active_tty_vfb) return;
    uint64_t flags = spinlock_acquire_irqsave(&t->lock);
    if (t->dirty) {
        tty_render_grid_to_vfb(t, g_active_tty_vfb);
        t->dirty = false;
    }
    extern void graphics_copy_buffer(uint32_t *src);
    tty_composite_cursor_vfb(t, g_active_tty_vfb);
    graphics_copy_buffer(g_active_tty_vfb);
    tty_restore_cursor_vfb(t, g_active_tty_vfb);
    spinlock_release_irqrestore(&t->lock, flags);
}

int tty_poll(int id, struct poll_table *pt) {
    if (pty_is_pty_id(id)) return pty_poll(id, pt);
    tty_t *t = tty_get(id);
    if (!t) return 0;
    
    int mask = 0;
    if (pt && pt->qproc) {
        pt->qproc(&t->char_queue.wait_queue, pt);
    }
    
    if (t->char_queue.head != t->char_queue.tail) {
        mask |= 0x0001; // POLLIN
    }
    
    mask |= 0x0004; // POLLOUT (always writable for now)
    
    return mask;
}
