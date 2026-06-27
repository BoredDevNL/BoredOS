// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "smp.h"
#include "limine.h"
#include "memory_manager.h"
#include "gdt.h"
#include "idt.h"
#include "platform.h"
#include "paging.h"
#include "process.h"
#include "work_queue.h"
#include "kutils.h"

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);
extern void serial_write_hex(uint64_t n);

static cpu_state_t *cpu_states = NULL;
static uint32_t total_cpus = 0;
static uint32_t bsp_lapic_id = 0;
static cpu_state_t *bsp_cpu_state = NULL;

#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static uint32_t read_lapic_id(void) {
    extern uint64_t hhdm_offset;
    volatile uint32_t *lapic = (volatile uint32_t *)(hhdm_offset + 0xFEE00000ULL);
    return (lapic[0x020 / 4] >> 24) & 0xFF;
}

uint32_t smp_this_cpu_id(void) {
    if (!cpu_states || total_cpus == 0) return 0;

    cpu_state_t *state = NULL;
    asm volatile("movq %%gs:0, %0" : "=r"(state) : : "memory");
    if (state && state >= cpu_states && state < cpu_states + total_cpus) {
        return state->cpu_id;
    }
    
    uint32_t lapic = read_lapic_id();
    for (uint32_t i = 0; i < total_cpus; i++) {
        if (cpu_states[i].online && cpu_states[i].lapic_id == lapic) return i;
    }
    
    return 0; 
}

uint32_t smp_cpu_count(void) {
    return total_cpus;
}

cpu_state_t *smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= total_cpus) return NULL;
    return &cpu_states[cpu_id];
}

static void ap_entry(struct limine_smp_info *info) {
    uint32_t my_id = (uint32_t)(info->extra_argument);
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1);
    cr0 |= (1ULL << 5);
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);
    cr4 |= (1ULL << 10);
    asm volatile("mov %0, %%cr4" : : "r"(cr4));
    asm volatile("fninit");

    extern struct gdt_ptr gdtr;
    extern void gdt_flush(uint64_t);
    gdt_flush((uint64_t)&gdtr);

    gdt_load_ap_tss(my_id);

    extern void idt_load(void);
    idt_load();

    extern void syscall_init(void);
    syscall_init();

    uint64_t kernel_cr3 = paging_get_pml4_phys();
    asm volatile("mov %0, %%cr3" : : "r"(kernel_cr3));

    extern void lapic_enable(void);
    lapic_enable();

    cpu_states[my_id].self = &cpu_states[my_id];
    cpu_states[my_id].online = true;
    cpu_states[my_id].kernel_syscall_stack = cpu_states[my_id].kernel_stack;

    wrmsr(MSR_GS_BASE, (uint64_t)&cpu_states[my_id]);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)&cpu_states[my_id]);

    serial_write("[SMP] AP ");
    serial_write_num(my_id);
    serial_write(" online (LAPIC ");
    serial_write_num(cpu_states[my_id].lapic_id);
    serial_write(")\n");

    process_t *ap_idle = process_create(NULL, false); 
    ap_idle->cpu_affinity = my_id;
    ap_idle->is_idle = true;
    strcpy(ap_idle->name, "idle:");
    char id_s[8]; itoa(my_id, id_s);
    strcpy(ap_idle->name + 5, id_s);
    
    process_set_current_for_cpu(my_id, ap_idle);
    asm volatile("sti");

    work_queue_drain_loop();
}

void smp_init_bsp(void) {
    static cpu_state_t bsp_state_static __attribute__((aligned(64))) = {0};
    bsp_state_static.cpu_id = 0;
    bsp_lapic_id = read_lapic_id();
    bsp_state_static.lapic_id = bsp_lapic_id;
    bsp_state_static.self = &bsp_state_static;
    bsp_state_static.online = true;
    
    wrmsr(MSR_GS_BASE, (uint64_t)&bsp_state_static);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)&bsp_state_static);
    
    bsp_cpu_state = &bsp_state_static;
}

// --- SMP Initialization ---
uint32_t smp_init(struct limine_smp_response *smp_resp) {
    if (!smp_resp || smp_resp->cpu_count <= 1) {
        total_cpus = 1;
        cpu_states = (cpu_state_t *)kmalloc_aligned(sizeof(cpu_state_t), 64);
        if (!cpu_states) return 1;
        memset(cpu_states, 0, sizeof(cpu_state_t));
        cpu_states[0].cpu_id = 0;
        cpu_states[0].lapic_id = read_lapic_id();
        cpu_states[0].online = true;
        serial_write("[SMP] Single CPU mode\n");
        return 1;
    }

    total_cpus = (uint32_t)smp_resp->cpu_count;
    bsp_lapic_id = smp_resp->bsp_lapic_id;

    serial_write("[SMP] Detected ");
    serial_write_num(total_cpus);
    serial_write(" CPUs. BSP LAPIC ID: ");
    serial_write_num(bsp_lapic_id);
    serial_write("\n");

    cpu_states = (cpu_state_t *)kmalloc_aligned(total_cpus * sizeof(cpu_state_t), 64);
    if (!cpu_states) {
        serial_write("[SMP] ERROR: Failed to allocate CPU state array!\n");
        total_cpus = 1;
        return 1;
    }
    memset(cpu_states, 0, total_cpus * sizeof(cpu_state_t));

    gdt_init_ap_tss(total_cpus);

    uint32_t bsp_index = 0;
    for (uint32_t i = 0; i < total_cpus; i++) {
        struct limine_smp_info *cpu = smp_resp->cpus[i];
        cpu_states[i].cpu_id = i;
        cpu_states[i].lapic_id = cpu->lapic_id;

        if (cpu->lapic_id == bsp_lapic_id) {
            cpu_states[i] = *bsp_cpu_state; // Copy early BSP state
            cpu_states[i].self = &cpu_states[i];
            
            cpu_states[i].kernel_stack = 0; // Limine stack for now
            cpu_states[i].kernel_syscall_stack = 0; 
            bsp_index = i;
            wrmsr(MSR_GS_BASE, (uint64_t)&cpu_states[i]);
            wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)&cpu_states[i]);
            
            serial_write("[SMP] BSP CPU ");
            serial_write_num(i);
            serial_write(" (LAPIC ");
            serial_write_num(cpu->lapic_id);
            serial_write(") online\n");
        } else {
            void *ap_stack = kmalloc_aligned(65536, 65536);
            if (!ap_stack) {
                serial_write("[SMP] ERROR: Failed to allocate AP stack!\n");
                continue;
            }
            cpu_states[i].kernel_stack = (uint64_t)ap_stack + 65536;
            cpu_states[i].kernel_stack_alloc = ap_stack;
            cpu_states[i].online = false;

            cpu->extra_argument = i;

            serial_write("[SMP] Starting AP ");
            serial_write_num(i);
            serial_write(" (LAPIC ");
            serial_write_num(cpu->lapic_id);
            serial_write(")...\n");

            __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);
        }
    }

    volatile uint32_t timeout = 10000000;
    uint32_t online_count = 0;
    while (timeout-- > 0) {
        online_count = 0;
        for (uint32_t i = 0; i < total_cpus; i++) {
            if (cpu_states[i].online) online_count++;
        }
        if (online_count == total_cpus) break;
        asm volatile("pause");
    }

    serial_write("[SMP] All ");
    serial_write_num(online_count);
    serial_write(" of ");
    serial_write_num(total_cpus);
    serial_write(" CPUs online\n");

    return online_count;
}

uint32_t smp_get_lapic_id(uint32_t cpu_id) {
    if (cpu_id >= total_cpus || !cpu_states) return 0xFF;
    return cpu_states[cpu_id].lapic_id;
}
