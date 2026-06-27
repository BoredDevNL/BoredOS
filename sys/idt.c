// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "idt.h"
#include "io.h"
#include "kutils.h"
#include "panic.h"

extern void serial_write(const char *str);

#include "process.h"
#include "cmd.h"

static const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

uint64_t exception_handler_c(registers_t *regs) {
    uint64_t vector = regs->int_no;
    char buf[17];
    
    // Serial Mirror
    serial_write("\n*** EXCEPTION ***\nVector: ");
    itoa_hex(vector, buf);
    serial_write("0x");
    serial_write(buf);
    
    if ((regs->cs & 0x3) != 0) {
        serial_write("\n*** USER MODE EXCEPTION ***\nVector: 0x");
        itoa_hex(vector, buf);
        serial_write(buf);
        serial_write("\nRIP: 0x");
        itoa_hex(regs->rip, buf);
        serial_write(buf);
        serial_write("\nError Code: 0x");
        itoa_hex(regs->err_code, buf);
        serial_write(buf);
        if (vector == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_write("\nCR2: 0x");
            itoa_hex(cr2, buf);
            serial_write(buf);
        }
        serial_write("\nTerminating process.\n");
        
        if (cmd_get_cursor_col() != 0) cmd_write("\n");
        cmd_write("*** USER EXCEPTION ***\nVector: "); cmd_write_hex(vector);
        cmd_write("\nRIP: "); cmd_write_hex(regs->rip);
        cmd_write("\nTerminating process.\n");
        int sig = 11; // Default to SIGSEGV
        if (vector == 0) sig = 8; // SIGFPE
        else if (vector == 6) sig = 4; // SIGILL
        return process_terminate_current_with_status(128 + sig, (uint64_t)regs);
    }

    // Kernel mode exception
    const char *name = (vector < 32) ? exception_messages[vector] : "Unknown Kernel Exception";
    serial_write("\nRIP: 0x"); itoa_hex(regs->rip, buf); serial_write(buf);
    serial_write("\nErr: 0x"); itoa_hex(regs->err_code, buf); serial_write(buf);
    if (vector == 14) { 
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        serial_write("\nCR2: 0x"); itoa_hex(cr2, buf); serial_write(buf);
    }
    serial_write("\n");
    kernel_panic(regs, name);
    
    return (uint64_t)regs; // Unreachable
}

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtr;

void idt_set_gate(uint8_t vector, void *isr, uint16_t cs, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;
    idt[vector].isr_low = addr & 0xFFFF;
    idt[vector].kernel_cs = cs; 
    idt[vector].ist = 0;
    idt[vector].attributes = flags;
    idt[vector].isr_mid = (addr >> 16) & 0xFFFF;
    idt[vector].isr_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved = 0;
}

// Remap PIC
static void pic_remap(void) {
    uint8_t a1, a2;
    a1 = inb(0x21);
    a2 = inb(0xA1);

    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); // Master offset 0x20 (32)
    outb(0xA1, 0x28); io_wait(); // Slave offset 0x28 (40)
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    // 0xEF = 1110 1111 (IRQ 12 (4 on slave) unmasked)
    
    outb(0x21, 0xF9); // Unmask Keyboard (IRQ1) and Cascade (IRQ2)
    outb(0xA1, 0xEF); // Unmask Mouse (IRQ12)
}

// Set up PIT (Programmable Interval Timer) for ~60Hz (16.67ms intervals)
static void pit_setup(void) {
    uint16_t divisor = 1193182 / 60;  // ~60Hz
    
    // Mode 2: Rate Generator (more appropriate for periodic interrupts)
    outb(0x43, 0x34); io_wait(); // Channel 0, lobyte/hibyte, mode 2, binary
    
    // Send divisor
    outb(0x40, divisor & 0xFF); io_wait();
    outb(0x40, (divisor >> 8) & 0xFF); io_wait();
}

void idt_init(void) {
    uint16_t cs;
    asm volatile ("mov %%cs, %0" : "=r"(cs));

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt[i] = (struct idt_entry){0};
    }

    pic_remap();
    
    // Unmask IRQ 0 (Timer) in addition to IRQ 1 and 12
    outb(0x21, 0xF8); // Unmask Timer (IRQ0), Keyboard (IRQ1) and Cascade (IRQ2)
    outb(0xA1, 0xEF); // Unmask Mouse (IRQ12)
    
    pit_setup();
}

void idt_register_interrupts(void) {
    uint16_t cs;
    asm volatile ("mov %%cs, %0" : "=r"(cs));
    
    idt_set_gate(32, isr0_wrapper, cs, 0x8E);  // Timer (IRQ 0)
    idt_set_gate(33, isr1_wrapper, cs, 0x8E);  // Keyboard (IRQ 1)
    idt_set_gate(44, isr12_wrapper, cs, 0x8E); // Mouse (IRQ 12)

    // Exceptions
    extern void exc0_wrapper(void);
    extern void exc1_wrapper(void);
    extern void exc2_wrapper(void);
    extern void exc3_wrapper(void);
    extern void exc4_wrapper(void);
    extern void exc5_wrapper(void);
    extern void exc6_wrapper(void);
    extern void exc7_wrapper(void);
    extern void exc8_wrapper(void);
    extern void exc9_wrapper(void);
    extern void exc10_wrapper(void);
    extern void exc11_wrapper(void);
    extern void exc12_wrapper(void);
    extern void exc13_wrapper(void);
    extern void exc14_wrapper(void);
    extern void exc15_wrapper(void);
    extern void exc16_wrapper(void);
    extern void exc17_wrapper(void);
    extern void exc18_wrapper(void);
    extern void exc19_wrapper(void);
    extern void exc20_wrapper(void);
    extern void exc21_wrapper(void);
    extern void exc22_wrapper(void);
    extern void exc23_wrapper(void);
    extern void exc24_wrapper(void);
    extern void exc25_wrapper(void);
    extern void exc26_wrapper(void);
    extern void exc27_wrapper(void);
    extern void exc28_wrapper(void);
    extern void exc29_wrapper(void);
    extern void exc30_wrapper(void);
    extern void exc31_wrapper(void);

    idt_set_gate(0,  exc0_wrapper,  cs, 0x8E);
    idt_set_gate(1,  exc1_wrapper,  cs, 0x8E);
    idt_set_gate(2,  exc2_wrapper,  cs, 0x8E);
    idt_set_gate(3,  exc3_wrapper,  cs, 0x8E);
    idt_set_gate(4,  exc4_wrapper,  cs, 0x8E);
    idt_set_gate(5,  exc5_wrapper,  cs, 0x8E);
    idt_set_gate(6,  exc6_wrapper,  cs, 0x8E);
    idt_set_gate(7,  exc7_wrapper,  cs, 0x8E);
    idt_set_gate(8,  exc8_wrapper,  cs, 0x8E);
    idt_set_gate(9,  exc9_wrapper,  cs, 0x8E);
    idt_set_gate(10, exc10_wrapper, cs, 0x8E);
    idt_set_gate(11, exc11_wrapper, cs, 0x8E);
    idt_set_gate(12, exc12_wrapper, cs, 0x8E);
    idt_set_gate(13, exc13_wrapper, cs, 0x8E);
    idt_set_gate(14, exc14_wrapper, cs, 0x8E);
    idt_set_gate(15, exc15_wrapper, cs, 0x8E);
    idt_set_gate(16, exc16_wrapper, cs, 0x8E);
    idt_set_gate(17, exc17_wrapper, cs, 0x8E);
    idt_set_gate(18, exc18_wrapper, cs, 0x8E);
    idt_set_gate(19, exc19_wrapper, cs, 0x8E);
    idt_set_gate(20, exc20_wrapper, cs, 0x8E);
    idt_set_gate(21, exc21_wrapper, cs, 0x8E);
    idt_set_gate(22, exc22_wrapper, cs, 0x8E);
    idt_set_gate(23, exc23_wrapper, cs, 0x8E);
    idt_set_gate(24, exc24_wrapper, cs, 0x8E);
    idt_set_gate(25, exc25_wrapper, cs, 0x8E);
    idt_set_gate(26, exc26_wrapper, cs, 0x8E);
    idt_set_gate(27, exc27_wrapper, cs, 0x8E);
    idt_set_gate(28, exc28_wrapper, cs, 0x8E);
    idt_set_gate(29, exc29_wrapper, cs, 0x8E);
    idt_set_gate(30, exc30_wrapper, cs, 0x8E);
    idt_set_gate(31, exc31_wrapper, cs, 0x8E);

    // SMP: Scheduling IPI for AP cores (vector 0x41 = 65)
    extern void isr_sched_ipi_wrapper(void);
    idt_set_gate(0x41, isr_sched_ipi_wrapper, cs, 0x8E);

    // PCI/ISA IRQ wrappers
    extern void isr5_wrapper(void);
    extern void isr9_wrapper(void);
    extern void isr10_wrapper(void);
    extern void isr11_wrapper(void);
    idt_set_gate(37, isr5_wrapper, cs, 0x8E);
    idt_set_gate(41, isr9_wrapper, cs, 0x8E);
    idt_set_gate(42, isr10_wrapper, cs, 0x8E);
    idt_set_gate(43, isr11_wrapper, cs, 0x8E);

    // Syscall Handler (vector 128) - DPL 3 for user access
    extern void isr128_wrapper(void);
    idt_set_gate(128, isr128_wrapper, cs, 0xEE);
}

void idt_load(void) {
    idtr.base = (uint64_t)&idt;
    idtr.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
    asm volatile ("lidt %0" : : "m"(idtr));
    // Do not sti here! The OS must decide when to enable interrupts
    // after all subsystems (WM, PS/2) are initialized!
}

static uint64_t (*irq_handlers[16])(registers_t *regs) = {0};

void idt_register_irq_handler(int irq, uint64_t (*handler)(registers_t *regs)) {
    if (irq >= 0 && irq < 16) {
        irq_handlers[irq] = handler;
    }
}

uint64_t irq_dispatch(int irq, registers_t *regs) {
    if (irq >= 0 && irq < 16 && irq_handlers[irq]) {
        return irq_handlers[irq](regs);
    }

    // Default EOI for unregistered IRQs
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
    return (uint64_t)regs;
}

uint64_t pci_irq_handler(registers_t *regs) {
    int irq = (int)regs->int_no - 32;
    return irq_dispatch(irq, regs);
}
