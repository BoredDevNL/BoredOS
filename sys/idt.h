// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idt_init(void);
void idt_set_gate(uint8_t vector, void *isr, uint16_t cs, uint8_t flags);
void idt_register_interrupts(void);
void idt_load(void);

struct registers_t;
void idt_register_irq_handler(int irq, uint64_t (*handler)(struct registers_t *regs));

// ISR wrappers defined in assembly
extern void isr0_wrapper(void);  // Timer
extern void isr1_wrapper(void);  // Keyboard
extern void isr12_wrapper(void); // Mouse

#endif
