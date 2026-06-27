#include "kconsole.h"
#include "graphics.h"
#include "spinlock.h"
#include <stddef.h>

static spinlock_t console_lock = SPINLOCK_INIT;
static int cursor_x = 0;
static int cursor_y = 0;
static bool kconsole_active = false;
static uint32_t text_color = 0xFFFFFFFF; // White

#define CHAR_WIDTH  8
#define CHAR_HEIGHT 10

void kconsole_init(void) {
    cursor_x = 10;
    cursor_y = 10;
    kconsole_active = false;
    
    // Initial clear screen during boot
    graphics_clear_back_buffer(0x00000000);
    graphics_mark_screen_dirty();
    graphics_flip_buffer();
}

void kconsole_set_active(bool active) {
    kconsole_active = active;
}

void kconsole_set_color(uint32_t color) {
    uint64_t flags = spinlock_acquire_irqsave(&console_lock);
    text_color = color;
    spinlock_release_irqrestore(&console_lock, flags);
}

static void kconsole_scroll(void) {
    if (cursor_y + CHAR_HEIGHT >= get_screen_height() - 10) {
        graphics_scroll_back_buffer(CHAR_HEIGHT);
        cursor_y -= CHAR_HEIGHT;
        graphics_mark_screen_dirty();
        graphics_flip_buffer();
    }
}

static void kconsole_putc_nolock(char c) {
    if (!kconsole_active) return;

    if (c == '\n') {
        cursor_x = 10;
        cursor_y += CHAR_HEIGHT;
        kconsole_scroll();
        return;
    }

    if (c == '\r') {
        cursor_x = 10;
        return;
    }

    if (c == '\t') {
        cursor_x += CHAR_WIDTH * 4;
        return;
    }

    // Draw character
    draw_char_bitmap(cursor_x, cursor_y, c, text_color);
    graphics_mark_screen_dirty();
    
    cursor_x += CHAR_WIDTH;
    if (cursor_x + CHAR_WIDTH >= get_screen_width() - 10) {
        cursor_x = 10;
        cursor_y += CHAR_HEIGHT;
        kconsole_scroll();
    }
}

void kconsole_putc(char c) {
    uint64_t flags = spinlock_acquire_irqsave(&console_lock);
    kconsole_putc_nolock(c);
    spinlock_release_irqrestore(&console_lock, flags);
}

void kconsole_write(const char *s) {
    if (!s) return;
    
    uint64_t flags = spinlock_acquire_irqsave(&console_lock);
    if (!kconsole_active) {
        spinlock_release_irqrestore(&console_lock, flags);
        return;
    }
    
    while (*s) {
        kconsole_putc_nolock(*s++);
    }
    spinlock_release_irqrestore(&console_lock, flags);
}
