// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "lapic.h"
#include "platform.h"
#include "spinlock.h"

extern void serial_write(const char *str);

// LAPIC is at physical 0xFEE00000. Access via HHDM.
static volatile uint32_t *lapic_base = 0;
static spinlock_t lapic_lock = SPINLOCK_INIT;

// LAPIC register offsets (byte offsets, divided by 4 for uint32_t* indexing)
#define LAPIC_ID        (0x020 / 4)
#define LAPIC_EOI       (0x0B0 / 4)
#define LAPIC_SVR       (0x0F0 / 4)
#define LAPIC_ICR_LOW   (0x300 / 4)
#define LAPIC_ICR_HIGH  (0x310 / 4)

void lapic_enable(void) {
    if (!lapic_base) return;
    // Enable the LAPIC by setting the Spurious Interrupt Vector Register
    // Bit 8 = APIC Software Enable, vector = 0xFF (spurious)
    lapic_base[LAPIC_SVR] = 0x1FF;
}

void lapic_init(void) {
    extern uint64_t hhdm_offset;
    lapic_base = (volatile uint32_t *)(hhdm_offset + 0xFEE00000ULL);
    
    lapic_enable();
    serial_write("[LAPIC] Initialized at HHDM + 0xFEE00000\n");
}

void lapic_eoi(void) {
    if (lapic_base) {
        lapic_base[LAPIC_EOI] = 0;
    }
}

void lapic_send_ipi_all(void) {
    if (!lapic_base) return;
    
    // Send IPI to all excluding self
    // ICR format:
    //   bits 7:0   = vector (IPI_SCHED_VECTOR = 0x41)
    //   bits 10:8  = delivery mode (000 = Fixed)
    //   bit 11     = destination mode (0 = Physical)
    //   bit 14     = level: 0 = Edge 
    //   bits 19:18 = destination shorthand (11 = All Excluding Self)
    uint32_t icr_low = IPI_SCHED_VECTOR | (0b11 << 18);
    
    uint64_t rflags = spinlock_acquire_irqsave(&lapic_lock);
    lapic_base[LAPIC_ICR_LOW] = icr_low;
    while (lapic_base[LAPIC_ICR_LOW] & (1 << 12)) {}
    spinlock_release_irqrestore(&lapic_lock, rflags);
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    if (!lapic_base) return;
    uint32_t icr_low = (uint32_t)vector; 

    uint64_t rflags = spinlock_acquire_irqsave(&lapic_lock);
    lapic_base[LAPIC_ICR_HIGH] = (lapic_id << 24);
    lapic_base[LAPIC_ICR_LOW] = icr_low;
    while (lapic_base[LAPIC_ICR_LOW] & (1 << 12)) {}
    spinlock_release_irqrestore(&lapic_lock, rflags);
}
