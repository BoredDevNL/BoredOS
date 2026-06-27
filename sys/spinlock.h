// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// Simple test-and-set spinlock for x86_64 SMP.
// Uses 'lock xchg' for acquire and a plain store for release.
// Includes 'pause' to reduce bus contention while spinning.

typedef volatile uint32_t spinlock_t;

#define SPINLOCK_INIT 0

static inline void spinlock_acquire(spinlock_t *lock) {
    while (1) {
        // Try to set the lock from 0 -> 1
        uint32_t prev;
        asm volatile("lock xchgl %0, %1"
                     : "=r"(prev), "+m"(*lock)
                     : "0"((uint32_t)1)
                     : "memory");
        if (prev == 0) return; // We got the lock
        // Spin with pause (reduces power and bus traffic)
        while (*lock) {
            asm volatile("pause" ::: "memory");
        }
    }
}

static inline void spinlock_release(spinlock_t *lock) {
    asm volatile("" ::: "memory"); // compiler barrier
    *lock = 0;
}

// Try to acquire without blocking. Returns 1 if acquired, 0 if not.
static inline int spinlock_try(spinlock_t *lock) {
    uint32_t prev;
    asm volatile("lock xchgl %0, %1"
                 : "=r"(prev), "+m"(*lock)
                 : "0"((uint32_t)1)
                 : "memory");
    return (prev == 0);
}

// IRQ-safe spinlock: saves flags, disables interrupts, then acquires.
// Use when the lock may be contended from interrupt context.
static inline uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spinlock_acquire(lock);
    return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_release(lock);
    asm volatile("push %0; popfq" : : "r"(flags) : "memory");
}

#endif
