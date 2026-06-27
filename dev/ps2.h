// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PS2_H
#define PS2_H

#define PS2_DATA_PORT       0x60
#define PS2_CMD_PORT        0x64
#define PS2_STATUS_PORT     0x64
#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX_DATA 0x20

#include <stdint.h>

void ps2_init(void);
#include "process.h"


uint64_t timer_handler(registers_t *regs);
uint64_t keyboard_handler(registers_t *regs);
uint64_t mouse_handler(registers_t *regs);

bool ps2_shift_pressed(void);

#endif
