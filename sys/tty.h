// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef TTY_H
#define TTY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"
#include "wait_queue.h"

#define TIOCGWINSZ 0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TTY_COUNT 10
#define TTY_IN_QUEUE_SIZE 1024

typedef struct {
    uint8_t buffer[TTY_IN_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    wait_queue_head_t wait_queue;
} tty_queue_t;

typedef struct {
    uint32_t codepoint;
    uint32_t fg;
    uint32_t bg;
} tty_cell_t;

typedef struct {
    int id;
    bool used;
    tty_cell_t *grid;
    bool dirty;
    int width, height;
    int cursor_x, cursor_y;
    bool cursor_visible;
    uint32_t fg_color, bg_color;
    bool blit_enabled;
    
    tty_queue_t key_queue;
    tty_queue_t mouse_queue;
    tty_queue_t out_queue; // For standard text output
    tty_queue_t char_queue; // For processed ASCII/UTF-8 input


    int fg_pid;
    uint32_t kb_mods;
    int esc_state;
    int esc_params[8];
    int esc_num_params;
    int saved_x, saved_y;
    int utf8_state;
    uint32_t utf8_codepoint;
    spinlock_t lock;
} tty_t;

void tty_init(void);
tty_t* tty_get(int id);
void tty_switch(int id);
int tty_get_active_id(void);

int tty_create(void);
int tty_destroy(int id);

void tty_write(int id, const char *data, size_t len);
void tty_write_output(int id, const char *data, size_t len);
int tty_read_output(int id, char *buf, size_t len);
int tty_write_input(int id, const char *buf, size_t len);

void tty_push_key(int id, uint8_t scancode);
void tty_push_mouse(int id, uint8_t *packet, size_t len);
int tty_read_key(int id, uint8_t *buf, size_t len);
int tty_read_mouse(int id, uint8_t *buf, size_t len);
void tty_push_char(int id, uint8_t c);
int tty_read_input(int id, char *buf, size_t len);

int tty_set_foreground(int id, int pid);
int tty_get_foreground(int id);

void tty_blit_active(void);
void tty_set_blit_enabled(bool enabled);
void tty_set_blit_enabled_for_id(int id, bool enabled);
bool tty_get_blit_enabled(void);
struct poll_table;
int tty_poll(int id, struct poll_table *pt);

#endif


