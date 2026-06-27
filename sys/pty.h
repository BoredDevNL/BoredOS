// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PTY_H
#define PTY_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"
#include "wait_queue.h"
#define PTY_MAX_COUNT 4096
#define PTY_QUEUE_SIZE 4096
#define PTY_ID_BASE 1024
typedef struct {
    uint8_t buffer[PTY_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    wait_queue_head_t wait_queue;
} pty_queue_t;
typedef struct {
    int id;
    bool used;
    pty_queue_t master_to_slave;
    pty_queue_t slave_to_master;
    int fg_pid;
    spinlock_t lock;
} pty_pair_t;
void pty_init(void);
int pty_create(void);
int pty_destroy(int pty_id);
pty_pair_t* pty_get(int pty_id);
bool pty_is_pty_id(int id);
void pty_write_output(int pty_id, const char *data, size_t len);
int pty_read_output(int pty_id, char *buf, size_t len);
int pty_write_input(int pty_id, const char *buf, size_t len);
int pty_read_input(int pty_id, char *buf, size_t len);
int pty_set_foreground(int pty_id, int pid);
int pty_get_foreground(int pty_id);
struct poll_table;
int pty_poll(int pty_id, struct poll_table *pt);

#endif
