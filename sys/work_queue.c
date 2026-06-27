// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "work_queue.h"
#include "spinlock.h"

extern void serial_write(const char *str);

#define WORK_QUEUE_SIZE 64

static work_item_t work_queue[WORK_QUEUE_SIZE];
static volatile int wq_head = 0;
static volatile int wq_tail = 0;
static spinlock_t wq_lock = SPINLOCK_INIT;

void work_queue_submit(work_fn_t fn, void *arg) {
    if (!fn) return;

    uint64_t flags = spinlock_acquire_irqsave(&wq_lock);
    int next_tail = (wq_tail + 1) % WORK_QUEUE_SIZE;
    if (next_tail == wq_head) {
        // Queue full — drop the work item
        spinlock_release_irqrestore(&wq_lock, flags);
        return;
    }
    work_queue[wq_tail].fn = fn;
    work_queue[wq_tail].arg = arg;
    wq_tail = next_tail;
    spinlock_release_irqrestore(&wq_lock, flags);
}

bool work_queue_drain_one(void) {
    uint64_t flags = spinlock_acquire_irqsave(&wq_lock);
    if (wq_head == wq_tail) {
        spinlock_release_irqrestore(&wq_lock, flags);
        return false;
    }
    work_item_t item = work_queue[wq_head];
    wq_head = (wq_head + 1) % WORK_QUEUE_SIZE;
    spinlock_release_irqrestore(&wq_lock, flags);

    // Execute outside the lock
    if (item.fn) {
        item.fn(item.arg);
    }
    return true;
}

void work_queue_drain_loop(void) {
    while (1) {
        // Try to drain all pending work
        while (work_queue_drain_one()) {
            // Keep draining
        }
        
        // No work — halt the CPU until an interrupt wakes us.
        // With legacy PIC, APs don't receive timer interrupts, so they'll
        // sleep until an IPI is sent (e.g., when work is submitted).
        // This is ideal: APs use 0% CPU when idle.
        asm volatile("sti; hlt; cli");
    }
}
