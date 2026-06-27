// Copyright (c) 2026 Lluciocc (https://github.com/lluciocc)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>
#include <stdbool.h>
#include "keycodes.h"

typedef enum {
    KEYMAP_QWERTY = 0,
    KEYMAP_AZERTY = 1,
    KEYMAP_QWERTZ = 2,
    KEYMAP_DVORAK = 3,
} keymap_id_t;

typedef struct {
    uint32_t normal;
    uint32_t shift;
    uint32_t altgr;
    uint32_t shift_altgr;
    uint8_t dead_mask;   // bit0 normal, bit1 shift, bit2 altgr, bit3 shift_altgr
    bool alpha;
} keymap_entry_t;

typedef struct {
    uint32_t codepoint;
    bool is_text;
    bool is_dead;
} keymap_result_t;

void keymap_init(void);
void keymap_set_current(keymap_id_t id);
keymap_id_t keymap_get_current(void);
const char *keymap_get_name(keymap_id_t id);
int keymap_get_count(void);

keymap_result_t keymap_translate_keycode(uint16_t keycode, uint32_t mods);
uint32_t keymap_compose(uint32_t dead_codepoint, uint32_t base_codepoint);

// compat legacy for existing apps
int keymap_legacy_key(uint16_t keycode, uint32_t codepoint);

#endif