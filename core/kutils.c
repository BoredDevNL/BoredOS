// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "kutils.h"
#include "io.h"

#include "acpi.h"

void page_zero_fast(void *page) {
    if (!page) return;
    uint8_t *p = (uint8_t *)page;
    size_t chunks = 4096 / 64;
    asm volatile(
        "pxor %%xmm0, %%xmm0\n\t"
        "1:\n\t"
        "movdqa %%xmm0, 0(%0)\n\t"
        "movdqa %%xmm0, 16(%0)\n\t"
        "movdqa %%xmm0, 32(%0)\n\t"
        "movdqa %%xmm0, 48(%0)\n\t"
        "add $64, %0\n\t"
        "dec %1\n\t"
        "jnz 1b\n\t"
        : "+r"(p), "+r"(chunks)
        :
        : "xmm0", "memory", "cc"
    );
}

void page_copy_fast(void *dest, const void *src) {
    if (!dest || !src) return;
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    size_t chunks = 4096 / 64;
    asm volatile(
        "1:\n\t"
        "movdqa 0(%1), %%xmm0\n\t"
        "movdqa 16(%1), %%xmm1\n\t"
        "movdqa 32(%1), %%xmm2\n\t"
        "movdqa 48(%1), %%xmm3\n\t"
        "movdqa %%xmm0, 0(%0)\n\t"
        "movdqa %%xmm1, 16(%0)\n\t"
        "movdqa %%xmm2, 32(%0)\n\t"
        "movdqa %%xmm3, 48(%0)\n\t"
        "add $64, %0\n\t"
        "add $64, %1\n\t"
        "dec %2\n\t"
        "jnz 1b\n\t"
        : "+r"(d), "+r"(s), "+r"(chunks)
        :
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc"
    );
}

void *memset(void *dest, int val, size_t len) {
    if (!dest || len == 0) return dest;

    uint8_t *d = (uint8_t *)dest;
    uint8_t val8 = (uint8_t)val;

    if (len >= 16) {
        while (len && ((uintptr_t)d & 15)) {
            *d++ = val8;
            len--;
        }

        uint64_t val64 = (uint64_t)val8 * 0x0101010101010101ULL;

        size_t vec_chunks = len / 64;
        if (vec_chunks > 0) {
            asm volatile(
                "movq %2, %%xmm0\n\t"
                "punpcklqdq %%xmm0, %%xmm0\n\t"
                "1:\n\t"
                "movdqa %%xmm0, 0(%0)\n\t"
                "movdqa %%xmm0, 16(%0)\n\t"
                "movdqa %%xmm0, 32(%0)\n\t"
                "movdqa %%xmm0, 48(%0)\n\t"
                "add $64, %0\n\t"
                "dec %1\n\t"
                "jnz 1b\n\t"
                : "+r"(d), "+r"(vec_chunks)
                : "r"(val64)
                : "xmm0", "memory", "cc"
            );
            len %= 64;
        }

        size_t xmm_chunks = len / 16;
        if (xmm_chunks > 0) {
            asm volatile(
                "movq %2, %%xmm0\n\t"
                "punpcklqdq %%xmm0, %%xmm0\n\t"
                "2:\n\t"
                "movdqa %%xmm0, 0(%0)\n\t"
                "add $16, %0\n\t"
                "dec %1\n\t"
                "jnz 2b\n\t"
                : "+r"(d), "+r"(xmm_chunks)
                : "r"(val64)
                : "xmm0", "memory", "cc"
            );
            len %= 16;
        }
    }

    size_t words = len / 8;
    if (words > 0 && (((uintptr_t)d & 7) == 0)) {
        uint64_t val64 = (uint64_t)val8 * 0x0101010101010101ULL;
        uint64_t *d64 = (uint64_t *)d;
        for (size_t i = 0; i < words; i++) {
            d64[i] = val64;
        }
        d += words * 8;
        len %= 8;
    }

    for (size_t i = 0; i < len; i++) {
        *d++ = val8;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t len) {
    if (!dest || !src || len == 0 || dest == src) return dest;

    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (len >= 16) {
        if ((((uintptr_t)d & 15) == 0) && (((uintptr_t)s & 15) == 0)) {
            size_t vec_chunks = len / 64;
            if (vec_chunks > 0) {
                asm volatile(
                    "1:\n\t"
                    "movdqa 0(%1), %%xmm0\n\t"
                    "movdqa 16(%1), %%xmm1\n\t"
                    "movdqa 32(%1), %%xmm2\n\t"
                    "movdqa 48(%1), %%xmm3\n\t"
                    "movdqa %%xmm0, 0(%0)\n\t"
                    "movdqa %%xmm1, 16(%0)\n\t"
                    "movdqa %%xmm2, 32(%0)\n\t"
                    "movdqa %%xmm3, 48(%0)\n\t"
                    "add $64, %0\n\t"
                    "add $64, %1\n\t"
                    "dec %2\n\t"
                    "jnz 1b\n\t"
                    : "+r"(d), "+r"(s), "+r"(vec_chunks)
                    :
                    : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc"
                );
                len %= 64;
            }

            size_t xmm_chunks = len / 16;
            if (xmm_chunks > 0) {
                asm volatile(
                    "2:\n\t"
                    "movdqa 0(%1), %%xmm0\n\t"
                    "movdqa %%xmm0, 0(%0)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 2b\n\t"
                    : "+r"(d), "+r"(s), "+r"(xmm_chunks)
                    :
                    : "xmm0", "memory", "cc"
                );
                len %= 16;
            }
        } else {
            size_t xmm_chunks = len / 16;
            if (xmm_chunks > 0) {
                asm volatile(
                    "3:\n\t"
                    "movdqu 0(%1), %%xmm0\n\t"
                    "movdqu %%xmm0, 0(%0)\n\t"
                    "add $16, %0\n\t"
                    "add $16, %1\n\t"
                    "dec %2\n\t"
                    "jnz 3b\n\t"
                    : "+r"(d), "+r"(s), "+r"(xmm_chunks)
                    :
                    : "xmm0", "memory", "cc"
                );
                len %= 16;
            }
        }
    }

    size_t words = len / 8;
    if (words > 0 && (((uintptr_t)d & 7) == 0) && (((uintptr_t)s & 7) == 0)) {
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        for (size_t i = 0; i < words; i++) {
            d64[i] = s64[i];
        }
        d += words * 8;
        s += words * 8;
        len %= 8;
    }

    for (size_t i = 0; i < len; i++) {
        *d++ = *s++;
    }
    return dest;
}

int memcmp(const void *str1, const void *str2, size_t count) {
    register const unsigned char *s1 = (const unsigned char*)str1;
    register const unsigned char *s2 = (const unsigned char*)str2;

    while (count-- > 0) {
        if (*s1++ != *s2++)
        return s1[-1] < s2[-1] ? -1 : 1;
    }
    return 0;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }

    return dest;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

void strcpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i = 0;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return dest;
}

char *strchr(const char *s, int c) {
    while (*s != (char)c) {
        if (!*s++) {
            return NULL;
        }
    }
    return (char *)s;
}

int atoi(const char *str) {
    int res = 0;
    int sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res * sign;
}

void itoa(int n, char *buf) {
    if (n == 0) {
        buf[0] = '0'; buf[1] = 0; return;
    }
    int i = 0;
    int sign = n < 0;
    if (sign) n = -n;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    if (sign) buf[i++] = '-';
    buf[i] = 0;
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}

void utoa(size_t n, char *buf) {
    if (n == 0) {
        buf[0] = '0'; buf[1] = 0; return;
    }
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }
    buf[i] = 0;
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}

void itoa_hex(uint64_t n, char *buf) {
    const char *digits = "0123456789ABCDEF";
    if (n == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    int i = 0;
    while (n > 0) {
        buf[i++] = digits[n & 0xF];
        n >>= 4;
    }
    buf[i] = 0;
    for (int j = 0; j < i / 2; j++) {
        char t = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = t;
    }
}

void itoa_hex32(uint32_t n, char *buf) {
    for (int j = 7; j >= 0; j--) {
        int digit = (n >> (j * 4)) & 0xF;
        *buf++ = digit < 10 ? '0' + digit : 'a' + (digit - 10);
    }
    *buf = 0;
}

void k_delay(int iterations) {
    for (volatile int i = 0; i < iterations; i++) {
        __asm__ __volatile__("nop");
    }
}

void k_sleep(int ms) {
    uint32_t ticks = (uint32_t)ms;
    if (ticks == 0 && ms > 0) ticks = 1;
    
    uint32_t target = get_ticks() + ticks;
    while (get_ticks() < target) {
        __asm__ __volatile__("hlt");
    }
}

void k_reboot(void) {
    outb(0x64, 0xFE);
}

void k_shutdown(void) {
    acpi_shutdown();
}

volatile uint64_t beep_end_tick = 0;
bool beep_active = false;

void k_beep(int freq, int ms) {
    if (freq <= 0) {
        outb(0x61, inb(0x61) & 0xFC);
        beep_active = false;
        return;
    }
    int div = 1193180 / freq;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, (div >> 8) & 0xFF);
    outb(0x61, inb(0x61) | 0x03);
    
    uint32_t ticks = (uint32_t)ms;
    if (ticks == 0 && ms > 0) ticks = 1;
    extern volatile uint64_t kernel_ticks;
    beep_end_tick = kernel_ticks + ticks;
    beep_active = true;
}

void k_beep_process(void) {
    if (beep_active) {
        extern volatile uint64_t kernel_ticks;
        if (kernel_ticks >= beep_end_tick) {
            outb(0x61, inb(0x61) & 0xFC);
            beep_active = false;
        }
    }
}

char *k_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

int text_encode_utf8(uint32_t cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    // Replacement character
    out[0] = (char)0xEF;
    out[1] = (char)0xBF;
    out[2] = (char)0xBD;
    return 3;
}

uint32_t get_ticks(void) {
    extern volatile uint64_t kernel_ticks;
    return (uint32_t)kernel_ticks;
}

char *strcat(char *dest, const char *src) {
    char *rdest = dest;
    while (*dest) dest++;
    while (*src) *dest++ = *src++;
    *dest = 0;
    return rdest;
}

bool str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return false;
    }
    return true;
}
