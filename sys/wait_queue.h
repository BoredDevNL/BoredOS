// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef WAIT_QUEUE_H
#define WAIT_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"

struct process;

typedef struct wait_queue_entry {
    struct process *proc;
    struct wait_queue_entry *next;
} wait_queue_entry_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef struct {
    wait_queue_entry_t *head;
    spinlock_t lock;
} wait_queue_head_t;

// Forward declaration of poll_table
struct poll_table;

typedef void (*poll_queue_proc)(wait_queue_head_t *h, struct poll_table *pt);

typedef struct poll_table {
    poll_queue_proc qproc;
} poll_table_t;

void wait_queue_init(wait_queue_head_t *h);
void wait_queue_add(wait_queue_head_t *h, wait_queue_entry_t *entry);
void wait_queue_remove(wait_queue_head_t *h, wait_queue_entry_t *entry);
void wait_queue_wake_all(wait_queue_head_t *h);

// --- Poll/Select Support ---
#define POLLIN      0x0001
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

#define MAX_POLL_ENTRIES 32

typedef struct {
    wait_queue_head_t *h;
    wait_queue_entry_t entry;
} poll_entry_t;

typedef struct {
    poll_table_t pt;
    poll_entry_t entries[MAX_POLL_ENTRIES];
    int count;
} poll_wtable_t;

#endif
