// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "wait_queue.h"
#include "process.h"
#include "memory_manager.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint64_t n);

void wait_queue_init(wait_queue_head_t *h) {
    if (!h) return;
    h->head = NULL;
    h->lock = SPINLOCK_INIT;
}

void wait_queue_add(wait_queue_head_t *h, wait_queue_entry_t *entry) {
    if (!h || !entry) return;
    uint64_t flags = spinlock_acquire_irqsave(&h->lock);
    
    // Prevent duplicate addition to avoid circular list corruption
    wait_queue_entry_t *curr = h->head;
    while (curr) {
        if (curr == entry) {
            spinlock_release_irqrestore(&h->lock, flags);
            return;
        }
        curr = curr->next;
    }
    
    entry->next = h->head;
    h->head = entry;
    spinlock_release_irqrestore(&h->lock, flags);
}

void wait_queue_remove(wait_queue_head_t *h, wait_queue_entry_t *entry) {
    if (!h || !entry) return;
    uint64_t flags = spinlock_acquire_irqsave(&h->lock);
    
    wait_queue_entry_t *prev = NULL;
    wait_queue_entry_t *curr = h->head;
    
    while (curr) {
        if (curr == entry) {
            if (prev) prev->next = curr->next;
            else h->head = curr->next;
            curr->next = NULL; // Clear next pointer for safety
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    
    spinlock_release_irqrestore(&h->lock, flags);
}

void wait_queue_wake_all(wait_queue_head_t *h) {
    if (!h) return;
    uint64_t flags = spinlock_acquire_irqsave(&h->lock);
    
    wait_queue_entry_t *curr = h->head;
    while (curr) {
        if (curr->proc) {
            curr->proc->state = PROC_STATE_RUNNING;
            curr->proc->sleep_until = 0;
        }
        curr = curr->next;
    }
    
    spinlock_release_irqrestore(&h->lock, flags);
}
