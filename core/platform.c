// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include <stdint.h>
#include "limine.h"
#include <stddef.h>
#include "platform.h"
#include "kutils.h"
static volatile struct limine_hhdm_request hhdm_request __attribute__((used, section(".requests"))) = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = NULL
};
static volatile struct limine_kernel_address_request kernel_addr_request __attribute__((used, section(".requests"))) = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = NULL
};
uint64_t hhdm_offset = 0;
uint64_t kernel_phys_base = 0;
uint64_t kernel_virt_base = 0;
void platform_init(void) {
    if (hhdm_request.response) { hhdm_offset = hhdm_request.response->offset; }
    if (kernel_addr_request.response) {
        kernel_phys_base = kernel_addr_request.response->physical_base;
        kernel_virt_base = kernel_addr_request.response->virtual_base;
    }

    // Enable FPU and SSE
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); // Clear EM (Emulation)
    cr0 |= (1ULL << 1);  // Set MP (Monitor Coprocessor)
    cr0 |= (1ULL << 5);  // Set NE (Numeric Error)
    asm volatile("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  // Set OSFXSR (FXSAVE/FXRSTOR support)
    cr4 |= (1ULL << 10); // Set OSXMMEXCPT (SIMD exception support)
    asm volatile("mov %0, %%cr4" : : "r"(cr4));

    // Initialize FPU
    asm volatile("fninit");
}
uint64_t p2v(uint64_t phys) { return phys + hhdm_offset; }
uint64_t v2p(uint64_t virt) {
    if (hhdm_offset && virt >= hhdm_offset) {
        if (!kernel_virt_base || virt < kernel_virt_base) {
            return virt - hhdm_offset;
        }
    }
    if (kernel_virt_base && virt >= kernel_virt_base) {
        return virt - kernel_virt_base + kernel_phys_base;
    }
    return virt;
}
void platform_get_cpu_model(char *model) {
    uint32_t brand[12];
    uint32_t eax, ebx, ecx, edx;

    for (uint32_t i = 0; i < 3; i++) {
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
        brand[i * 4 + 0] = eax;
        brand[i * 4 + 1] = ebx;
        brand[i * 4 + 2] = ecx;
        brand[i * 4 + 3] = edx;
    }
    
    char *p = (char *)brand;
    for (int i = 0; i < 48; i++) {
        model[i] = p[i];
    }
    model[48] = '\0';
}
void platform_get_cpu_vendor(char *vendor) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    
    char *p = (char *)vendor;
    *((uint32_t *)&p[0]) = ebx;
    *((uint32_t *)&p[4]) = edx;
    *((uint32_t *)&p[8]) = ecx;
    p[12] = '\0';
}

void platform_get_cpu_info(cpu_info_t *info) {
    uint32_t eax, ebx, ecx, edx;
    
    // CPUID leaf 1: basic feature information
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    info->stepping = eax & 0xF;
    info->model = (eax >> 4) & 0xF;
    info->family = (eax >> 8) & 0xF;
    info->microcode = (ebx >> 8) & 0xFF;
    info->flags = ((uint64_t)ecx << 32) | edx;  // ECX and EDX contain feature flags
    info->cache_size = (ebx >> 16) & 0xFF;      // Cache line size in bytes
}

void platform_get_cpu_flags(char *flags_str) {
    uint32_t eax, ebx, ecx, edx;
    
    flags_str[0] = '\0';
    
    // CPUID leaf 1
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    
    // ECX flags
    if (ecx & (1 << 0)) strcpy(flags_str + strlen(flags_str), "sse3 ");
    if (ecx & (1 << 1)) strcpy(flags_str + strlen(flags_str), "pclmulqdq ");
    if (ecx & (1 << 3)) strcpy(flags_str + strlen(flags_str), "monitor ");
    if (ecx & (1 << 6)) strcpy(flags_str + strlen(flags_str), "ssse3 ");
    if (ecx & (1 << 9)) strcpy(flags_str + strlen(flags_str), "sdbg ");
    if (ecx & (1 << 12)) strcpy(flags_str + strlen(flags_str), "fma ");
    if (ecx & (1 << 13)) strcpy(flags_str + strlen(flags_str), "cx16 ");
    if (ecx & (1 << 19)) strcpy(flags_str + strlen(flags_str), "sse4_1 ");
    if (ecx & (1 << 20)) strcpy(flags_str + strlen(flags_str), "sse4_2 ");
    if (ecx & (1 << 23)) strcpy(flags_str + strlen(flags_str), "popcnt ");
    if (ecx & (1 << 25)) strcpy(flags_str + strlen(flags_str), "aes ");
    if (ecx & (1 << 26)) strcpy(flags_str + strlen(flags_str), "xsave ");
    if (ecx & (1 << 28)) strcpy(flags_str + strlen(flags_str), "avx ");
    
    // EDX flags
    if (edx & (1 << 0)) strcpy(flags_str + strlen(flags_str), "fpu ");
    if (edx & (1 << 3)) strcpy(flags_str + strlen(flags_str), "pse ");
    if (edx & (1 << 4)) strcpy(flags_str + strlen(flags_str), "tsc ");
    if (edx & (1 << 6)) strcpy(flags_str + strlen(flags_str), "pae ");
    if (edx & (1 << 8)) strcpy(flags_str + strlen(flags_str), "cx8 ");
    if (edx & (1 << 9)) strcpy(flags_str + strlen(flags_str), "apic ");
    if (edx & (1 << 11)) strcpy(flags_str + strlen(flags_str), "sep ");
    if (edx & (1 << 15)) strcpy(flags_str + strlen(flags_str), "cmov ");
    if (edx & (1 << 23)) strcpy(flags_str + strlen(flags_str), "mmx ");
    if (edx & (1 << 24)) strcpy(flags_str + strlen(flags_str), "fxsr ");
    if (edx & (1 << 25)) strcpy(flags_str + strlen(flags_str), "sse ");
    if (edx & (1 << 26)) strcpy(flags_str + strlen(flags_str), "sse2 ");
    
    // Extended leaf 0x80000001 for advanced flags
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    
    if (edx & (1 << 11)) strcpy(flags_str + strlen(flags_str), "syscall ");
    if (edx & (1 << 20)) strcpy(flags_str + strlen(flags_str), "nx ");
    if (edx & (1 << 26)) strcpy(flags_str + strlen(flags_str), "pdpe1gb ");
    if (edx & (1 << 27)) strcpy(flags_str + strlen(flags_str), "rdtscp ");
    if (edx & (1 << 29)) strcpy(flags_str + strlen(flags_str), "lm ");
    
    if (ecx & (1 << 0)) strcpy(flags_str + strlen(flags_str), "lahf_lm ");
    if (ecx & (1 << 5)) strcpy(flags_str + strlen(flags_str), "abm ");
    
    // Remove trailing space
    int len = strlen(flags_str);
    if (len > 0 && flags_str[len-1] == ' ') {
        flags_str[len-1] = '\0';
    }
}
