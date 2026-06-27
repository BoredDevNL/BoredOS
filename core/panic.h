#ifndef PANIC_H
#define PANIC_H

#include "io.h"
#include "kutils.h"
#include "syscall.h"

void kernel_panic(registers_t *regs, const char *error_name);

#endif