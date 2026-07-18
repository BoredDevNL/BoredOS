// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>
#include "spinlock.h"

typedef struct cpu_state {
    struct cpu_state *self;    
    uint32_t cpu_id;           
    uint32_t lapic_id;         
    uint64_t kernel_stack;     
    void    *kernel_stack_alloc; 
    volatile bool online;      
    uint64_t user_rsp_scratch;  
    uint64_t kernel_syscall_stack; 
    uint8_t xsave_area[8192] __attribute__((aligned(64)));
    void *current_process;
} cpu_state_t;
 
 void smp_init_bsp(void);


struct limine_smp_response;
uint32_t smp_init(struct limine_smp_response *smp_resp);

uint32_t smp_this_cpu_id(void);

uint32_t smp_cpu_count(void);

cpu_state_t *smp_get_cpu(uint32_t cpu_id);

uint32_t smp_get_lapic_id(uint32_t cpu_id);

#endif
