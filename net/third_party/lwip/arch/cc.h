#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>
#include "memory_manager.h"

#define malloc  kmalloc
#define free    kfree
// Correctly map calloc to kcalloc to ensure memory is zero-initialized.
// Previously this was kmalloc(n*s), which left garbage in lwIP structures.
#define calloc  kcalloc
#define realloc krealloc

/* Define generic types used in lwIP */
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;

typedef uintptr_t mem_ptr_t;
typedef uint32_t sys_prot_t;

#define LWIP_ERR_T int

/* Define (sn)printf formatters */
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_STDINT_H   1
#define LWIP_NO_CTYPE_H    1
#define LWIP_NO_LIMITS_H   0

/* Compiler specific symbols */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__ ((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

extern void serial_write(const char *str);
#define LWIP_PLATFORM_DIAG(x) do { \
    extern void serial_write(const char *str); \
    serial_write x; \
} while(0)
#define LWIP_PLATFORM_ASSERT(x) do { serial_write("ASSERT FAILED: "); serial_write(x); serial_write("\n"); } while(0)

extern uint32_t sys_now(void);
#define LWIP_RAND() ((u32_t)sys_now())

extern sys_prot_t sys_arch_protect(void);
extern void sys_arch_unprotect(sys_prot_t pval);

#endif /* LWIP_ARCH_CC_H */
