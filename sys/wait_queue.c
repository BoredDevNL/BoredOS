// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "wait_queue.h"
#include "process.h"
#include "memory_manager.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint64_t n);

static bool is_valid_kernel_ptr(const void *ptr) {
    if (!ptr) return false;
    uint64_t addr = (uint64_t)ptr;
    if ((addr & 0xFFFF800000000000ULL) != 0xFFFF800000000000ULL) return false;
    if (addr < 0xFFFF800000000000ULL || addr > 0xFFFFFFFFFFFFF000ULL) return false;
    extern bool mm_is_heap_address(void *p);
    if (addr < 0xFFFFFFFF80000000ULL && !mm_is_heap_address((void *)ptr)) return false;
    return true;
}

void wait_queue_init(wait_queue_head_t *h) {
    if (!is_valid_kernel_ptr(h)) return;
    h->head = NULL;
    h->lock = SPINLOCK_INIT;
}

void wait_queue_add(wait_queue_head_t *h, wait_queue_entry_t *entry) {
    if (!is_valid_kernel_ptr(h) || !is_valid_kernel_ptr(entry)) return;
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
    if (!is_valid_kernel_ptr(h) || !is_valid_kernel_ptr(entry)) return;
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
    if (!is_valid_kernel_ptr(h)) return;
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

void wait_queue_wait(wait_queue_head_t *h) {
    if (!h) return;
    process_t *curr = process_get_current();
    if (curr) {
        curr->wait_node.proc = curr;
        curr->wait_node.next = NULL;
        wait_queue_add(h, &curr->wait_node);
        curr->state = PROC_STATE_BLOCKED;
        asm volatile("int $0x20");
        wait_queue_remove(h, &curr->wait_node);
    }
}

void wait_queue_wait_timeout(wait_queue_head_t *h, uint32_t timeout_ms) {
    if (!h) return;
    process_t *curr = process_get_current();
    if (curr) {
        curr->wait_node.proc = curr;
        curr->wait_node.next = NULL;
        wait_queue_add(h, &curr->wait_node);
        curr->state = PROC_STATE_BLOCKED;
        if (timeout_ms > 0) {
            extern uint64_t get_ticks(void);
            curr->sleep_until = get_ticks() + timeout_ms;
        }
        asm volatile("int $0x20");
        curr->sleep_until = 0;
        wait_queue_remove(h, &curr->wait_node);
    }
}
