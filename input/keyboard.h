// Copyright (c) 2026 Lluciocc (https://github.com/lluciocc)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef KEYBOARD_H
#define KEYBOARD_H

#define KBD_DATA_PORT       0x60
#define KBD_CMD_PORT        0x64

#include <stdint.h>
#include <stdbool.h>
#include "keycodes.h"

typedef struct {
    uint16_t keycode;
    uint32_t codepoint;
    uint32_t mods;
    bool pressed;
    bool repeat;
    bool is_text;
} keyboard_event_t;

void keyboard_init(void);
bool keyboard_handle_set1_scancode(uint8_t scancode, keyboard_event_t *ev);

uint16_t keyboard_keycode_from_set1(uint8_t scancode, bool extended);

bool keyboard_shift_pressed(void);
bool keyboard_ctrl_pressed(void);
uint32_t keyboard_get_modifiers(void);

#endif