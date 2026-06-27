#include <stdint.h>
#include "lwip/opt.h"
#include "lwip/arch.h"

extern volatile uint64_t kernel_ticks;

uint32_t sys_now(void) {

    return (uint32_t)(kernel_ticks * 1000 / 60);
}


