// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"

// A simple work queue for offloading tasks to idle AP cores.
// Producer (BSP or any core) calls work_queue_submit().
// Consumer (AP idle loops) calls work_queue_drain_loop().

typedef void (*work_fn_t)(void *arg);

typedef struct {
    work_fn_t fn;
    void *arg;
} work_item_t;

// Submit a work item. Thread-safe (uses spinlock internally).
void work_queue_submit(work_fn_t fn, void *arg);

// Drain and execute all pending work items, then hlt until more arrive.
// Called from AP idle loops. Never returns.
void work_queue_drain_loop(void);

// Drain one item (if available). Returns true if work was done.
// Useful for BSP to optionally help if idle.
bool work_queue_drain_one(void);

#endif
