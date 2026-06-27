// Copyright (c) 2026 Lluciocc (https://github.com/lluciocc)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "keymap.h"

#define DEAD_NORMAL       0x01
#define DEAD_SHIFT        0x02
#define DEAD_ALTGR        0x04
#define DEAD_SHIFT_ALTGR  0x08

typedef struct {
    const char *name;
    keymap_entry_t entries[KEY_MAX];
} keyboard_layout_t;

typedef struct {
    uint32_t dead;
    uint32_t base;
    uint32_t result;
} compose_entry_t;

/*
HOW TO ADD A NEW LAYOUT:
1. Add a new entry to the keymap_id_t enum in keymap.h
2. Create a new keyboard_layout_t instance in keymap.c, filling the entries array with the appropriate codepoints for each keycode and modifier combination. Use 0 for unused combinations.
3. Add the new layout to the g_layouts array in keymap.c
4. (Optional) If your layout has dead keys, add the appropriate entries to the g_compose_table array in keymap.c, defining how dead keys combine with base characters to produce composed characters.
*/

// QWERTY LAYOUT US (DEFAULT)
static const keyboard_layout_t layout_qwerty = {
    "QWERTY (US)",
    .entries = {
        // [KEYCODE] = {normal, shift, altgr, shift_altgr, dead_mask, alpha}
        [KEY_1] = {'1', '!', 0, 0, 0, false},
        [KEY_2] = {'2', '@', 0, 0, 0, false},
        [KEY_3] = {'3', '#', 0, 0, 0, false},
        [KEY_4] = {'4', '$', 0, 0, 0, false},
        [KEY_5] = {'5', '%', 0, 0, 0, false},
        [KEY_6] = {'6', '^', 0, 0, 0, false},
        [KEY_7] = {'7', '&', 0, 0, 0, false},
        [KEY_8] = {'8', '*', 0, 0, 0, false},
        [KEY_9] = {'9', '(', 0, 0, 0, false},
        [KEY_0] = {'0', ')', 0, 0, 0, false},
        [KEY_MINUS] = {'-', '_', 0, 0, 0, false},
        [KEY_EQUAL] = {'=', '+', 0, 0, 0, false},

        [KEY_Q] = {'q', 'Q', 0, 0, 0, true},
        [KEY_W] = {'w', 'W', 0, 0, 0, true},
        [KEY_E] = {'e', 'E', 0, 0, 0, true},
        [KEY_R] = {'r', 'R', 0, 0, 0, true},
        [KEY_T] = {'t', 'T', 0, 0, 0, true},
        [KEY_Y] = {'y', 'Y', 0, 0, 0, true},
        [KEY_U] = {'u', 'U', 0, 0, 0, true},
        [KEY_I] = {'i', 'I', 0, 0, 0, true},
        [KEY_O] = {'o', 'O', 0, 0, 0, true},
        [KEY_P] = {'p', 'P', 0, 0, 0, true},
        [KEY_LBRACKET] = {'[', '{', 0, 0, 0, false},
        [KEY_RBRACKET] = {']', '}', 0, 0, 0, false},

        [KEY_A] = {'a', 'A', 0, 0, 0, true},
        [KEY_S] = {'s', 'S', 0, 0, 0, true},
        [KEY_D] = {'d', 'D', 0, 0, 0, true},
        [KEY_F] = {'f', 'F', 0, 0, 0, true},
        [KEY_G] = {'g', 'G', 0, 0, 0, true},
        [KEY_H] = {'h', 'H', 0, 0, 0, true},
        [KEY_J] = {'j', 'J', 0, 0, 0, true},
        [KEY_K] = {'k', 'K', 0, 0, 0, true},
        [KEY_L] = {'l', 'L', 0, 0, 0, true},
        [KEY_SEMICOLON] = {';', ':', 0, 0, 0, false},
        [KEY_APOSTROPHE] = {'\'', '"', 0, 0, 0, false},
        [KEY_GRAVE] = {'`', '~', 0, 0, 0, false},

        [KEY_BACKSLASH] = {'\\', '|', 0, 0, 0, false},
        [KEY_Z] = {'z', 'Z', 0, 0, 0, true},
        [KEY_X] = {'x', 'X', 0, 0, 0, true},
        [KEY_C] = {'c', 'C', 0, 0, 0, true},
        [KEY_V] = {'v', 'V', 0, 0, 0, true},
        [KEY_B] = {'b', 'B', 0, 0, 0, true},
        [KEY_N] = {'n', 'N', 0, 0, 0, true},
        [KEY_M] = {'m', 'M', 0, 0, 0, true},
        [KEY_COMMA] = {',', '<', 0, 0, 0, false},
        [KEY_DOT] = {'.', '>', 0, 0, 0, false},
        [KEY_SLASH] = {'/', '?', 0, 0, 0, false},

        [KEY_SPACE] = {' ', ' ', 0, 0, 0, false},
        [KEY_KP_SLASH] = {'/', '/', 0, 0, 0, false},
        [KEY_KP_STAR] = {'*', '*', 0, 0, 0, false},
        [KEY_KP_MINUS] = {'-', '-', 0, 0, 0, false},
        [KEY_KP_PLUS] = {'+', '+', 0, 0, 0, false},
        [KEY_KP_DOT] = {'.', '.', 0, 0, 0, false},
        [KEY_KP_0] = {'0', '0', 0, 0, 0, false},
        [KEY_KP_1] = {'1', '1', 0, 0, 0, false},
        [KEY_KP_2] = {'2', '2', 0, 0, 0, false},
        [KEY_KP_3] = {'3', '3', 0, 0, 0, false},
        [KEY_KP_4] = {'4', '4', 0, 0, 0, false},
        [KEY_KP_5] = {'5', '5', 0, 0, 0, false},
        [KEY_KP_6] = {'6', '6', 0, 0, 0, false},
        [KEY_KP_7] = {'7', '7', 0, 0, 0, false},
        [KEY_KP_8] = {'8', '8', 0, 0, 0, false},
        [KEY_KP_9] = {'9', '9', 0, 0, 0, false},
    }
};

// AZERTY LAYOUT FR
static const keyboard_layout_t layout_azerty = {
    "AZERTY (FR)",
    .entries = {
        [KEY_1] = { '&', '1', 0, 0, 0, false },
        [KEY_2] = { 0x00E9, '2', '~', 0, 0, false },         // é / 2 / ~
        [KEY_3] = { '"', '3', '#', 0, 0, false },
        [KEY_4] = { '\'', '4', '{', 0, 0, false },
        [KEY_5] = { '(', '5', '[', 0, 0, false },
        [KEY_6] = { '-', '6', '|', 0, 0, false },
        [KEY_7] = { 0x00E8, '7', '`', 0, 0, false },         // è / 7 / `
        [KEY_8] = { '_', '8', '\\', 0, 0, false },
        [KEY_9] = { 0x00E7, '9', '^', 0, 0, false },         // ç / 9 / ^
        [KEY_0] = { 0x00E0, '0', '@', 0, 0, false },         // à / 0 / @
        [KEY_MINUS] = { ')', 0x00B0, ']', 0, 0, false },     // ) / °
        [KEY_EQUAL] = { '=', '+', '}', 0, 0, false },

        [KEY_Q] = { 'a', 'A', 0, 0, 0, true },
        [KEY_W] = { 'z', 'Z', 0, 0, 0, true },
        [KEY_E] = { 'e', 'E', 0x20AC, 0, 0, true },          // €
        [KEY_R] = { 'r', 'R', 0, 0, 0, true },
        [KEY_T] = { 't', 'T', 0, 0, 0, true },
        [KEY_Y] = { 'y', 'Y', 0, 0, 0, true },
        [KEY_U] = { 'u', 'U', 0, 0, 0, true },
        [KEY_I] = { 'i', 'I', 0, 0, 0, true },
        [KEY_O] = { 'o', 'O', 0, 0, 0, true },
        [KEY_P] = { 'p', 'P', 0, 0, 0, true },
        [KEY_LBRACKET] = { '^', 0x00A8, 0, 0, DEAD_NORMAL | DEAD_SHIFT, false }, // ^ / ¨
        [KEY_RBRACKET] = { '$', 0x00A3, 0, 0, 0, false },    // £

        [KEY_A] = { 'q', 'Q', 0, 0, 0, true },
        [KEY_S] = { 's', 'S', 0, 0, 0, true },
        [KEY_D] = { 'd', 'D', 0, 0, 0, true },
        [KEY_F] = { 'f', 'F', 0, 0, 0, true },
        [KEY_G] = { 'g', 'G', 0, 0, 0, true },
        [KEY_H] = { 'h', 'H', 0, 0, 0, true },
        [KEY_J] = { 'j', 'J', 0, 0, 0, true },
        [KEY_K] = { 'k', 'K', 0, 0, 0, true },
        [KEY_L] = { 'l', 'L', 0, 0, 0, true },
        [KEY_SEMICOLON] = { 'm', 'M', 0, 0, 0, true },
        [KEY_APOSTROPHE] = { 0x00F9, '%', 0, 0, 0, false },  // ù / %
        [KEY_GRAVE] = { 0x00B2, 0, 0, 0, 0, false },         // ²

        [KEY_BACKSLASH] = { '*', 0x00B5, 0, 0, 0, false },   // * / µ
        [KEY_Z] = { 'w', 'W', 0, 0, 0, true },
        [KEY_X] = { 'x', 'X', 0, 0, 0, true },
        [KEY_C] = { 'c', 'C', 0, 0, 0, true },
        [KEY_V] = { 'v', 'V', 0, 0, 0, true },
        [KEY_B] = { 'b', 'B', 0, 0, 0, true },
        [KEY_N] = { 'n', 'N', 0, 0, 0, true },
        [KEY_M] = { ',', '?', 0, 0, 0, false },
        [KEY_COMMA] = { ';', '.', 0, 0, 0, false },
        [KEY_DOT] = { ':', '/', 0, 0, 0, false },
        [KEY_SLASH] = { '!', 0x00A7, 0, 0, 0, false },

        [KEY_SPACE] = { ' ', ' ', 0, 0, 0, false },

        [KEY_KP_SLASH] = {'/', '/', 0, 0, 0, false},
        [KEY_KP_STAR] = {'*', '*', 0, 0, 0, false},
        [KEY_KP_MINUS] = {'-', '-', 0, 0, 0, false},
        [KEY_KP_PLUS] = {'+', '+', 0, 0, 0, false},
        [KEY_KP_DOT] = {'.', '.', 0, 0, 0, false},
        [KEY_KP_0] = {'0', '0', 0, 0, 0, false},
        [KEY_KP_1] = {'1', '1', 0, 0, 0, false},
        [KEY_KP_2] = {'2', '2', 0, 0, 0, false},
        [KEY_KP_3] = {'3', '3', 0, 0, 0, false},
        [KEY_KP_4] = {'4', '4', 0, 0, 0, false},
        [KEY_KP_5] = {'5', '5', 0, 0, 0, false},
        [KEY_KP_6] = {'6', '6', 0, 0, 0, false},
        [KEY_KP_7] = {'7', '7', 0, 0, 0, false},
        [KEY_KP_8] = {'8', '8', 0, 0, 0, false},
        [KEY_KP_9] = {'9', '9', 0, 0, 0, false},
    }
};

static const keyboard_layout_t layout_qwertz = {
    "QWERTZ (DE)",
    .entries = {
        [KEY_1] = { '1', '!', 0, 0, 0, false },
        [KEY_2] = { '2', '"', 0x00B2, 0, 0, false },        // ²
        [KEY_3] = { '3', 0x00A7, 0x00B3, 0, 0, false },     // § ³
        [KEY_4] = { '4', '$', 0, 0, 0, false },
        [KEY_5] = { '5', '%', 0, 0, 0, false },
        [KEY_6] = { '6', '&', 0, 0, 0, false },
        [KEY_7] = { '7', '/', '{', 0, 0, false },
        [KEY_8] = { '8', '(', '[', 0, 0, false },
        [KEY_9] = { '9', ')', ']', 0, 0, false },
        [KEY_0] = { '0', '=', '}', 0, 0, false },
        [KEY_MINUS] = { 0x00DF, '?', '\\', 0, 0, false },   // ß
        [KEY_EQUAL] = { 0x00B4, '`', 0, 0, DEAD_NORMAL, false }, // ´ dead

        [KEY_Q] = { 'q', 'Q', '@', 0, 0, true },
        [KEY_W] = { 'w', 'W', 0, 0, 0, true },
        [KEY_E] = { 'e', 'E', 0x20AC, 0, 0, true },         // €
        [KEY_R] = { 'r', 'R', 0, 0, 0, true },
        [KEY_T] = { 't', 'T', 0, 0, 0, true },
        [KEY_Y] = { 'z', 'Z', 0, 0, 0, true },
        [KEY_U] = { 'u', 'U', 0, 0, 0, true },
        [KEY_I] = { 'i', 'I', 0, 0, 0, true },
        [KEY_O] = { 'o', 'O', 0, 0, 0, true },
        [KEY_P] = { 'p', 'P', 0, 0, 0, true },
        [KEY_LBRACKET] = { 0x00FC, 0x00DC, 0, 0, 0, true }, // ü
        [KEY_RBRACKET] = { '+', '*', '~', 0, 0, false },

        [KEY_A] = { 'a', 'A', 0, 0, 0, true },
        [KEY_S] = { 's', 'S', 0, 0, 0, true },
        [KEY_D] = { 'd', 'D', 0, 0, 0, true },
        [KEY_F] = { 'f', 'F', 0, 0, 0, true },
        [KEY_G] = { 'g', 'G', 0, 0, 0, true },
        [KEY_H] = { 'h', 'H', 0, 0, 0, true },
        [KEY_J] = { 'j', 'J', 0, 0, 0, true },
        [KEY_K] = { 'k', 'K', 0, 0, 0, true },
        [KEY_L] = { 'l', 'L', 0, 0, 0, true },
        [KEY_SEMICOLON] = { 0x00F6, 0x00D6, 0, 0, 0, true }, // ö
        [KEY_APOSTROPHE] = { 0x00E4, 0x00C4, 0, 0, 0, true }, // ä
        [KEY_GRAVE] = { '^', 0x00B0, 0, 0, DEAD_NORMAL, false }, // ^ dead

        [KEY_BACKSLASH] = { '#', '\'', 0, 0, 0, false },
        [KEY_Z] = { 'y', 'Y', 0, 0, 0, true },
        [KEY_X] = { 'x', 'X', 0, 0, 0, true },
        [KEY_C] = { 'c', 'C', 0, 0, 0, true },
        [KEY_V] = { 'v', 'V', 0, 0, 0, true },
        [KEY_B] = { 'b', 'B', 0, 0, 0, true },
        [KEY_N] = { 'n', 'N', 0, 0, 0, true },
        [KEY_M] = { 'm', 'M', 0, 0, 0, true },
        [KEY_COMMA] = { ',', ';', 0, 0, 0, false },
        [KEY_DOT] = { '.', ':', 0, 0, 0, false },
        [KEY_SLASH] = { '-', '_', 0, 0, 0, false },

        [KEY_SPACE] = { ' ', ' ', 0, 0, 0, false },

        [KEY_KP_SLASH] = {'/', '/', 0, 0, 0, false},
        [KEY_KP_STAR] = {'*', '*', 0, 0, 0, false},
        [KEY_KP_MINUS] = {'-', '-', 0, 0, 0, false},
        [KEY_KP_PLUS] = {'+', '+', 0, 0, 0, false},
        [KEY_KP_DOT] = {'.', '.', 0, 0, 0, false},
        [KEY_KP_0] = {'0', '0', 0, 0, 0, false},
        [KEY_KP_1] = {'1', '1', 0, 0, 0, false},
        [KEY_KP_2] = {'2', '2', 0, 0, 0, false},
        [KEY_KP_3] = {'3', '3', 0, 0, 0, false},
        [KEY_KP_4] = {'4', '4', 0, 0, 0, false},
        [KEY_KP_5] = {'5', '5', 0, 0, 0, false},
        [KEY_KP_6] = {'6', '6', 0, 0, 0, false},
        [KEY_KP_7] = {'7', '7', 0, 0, 0, false},
        [KEY_KP_8] = {'8', '8', 0, 0, 0, false},
        [KEY_KP_9] = {'9', '9', 0, 0, 0, false},
    }
};

static const keyboard_layout_t layout_dvorak = {
    "DVORAK",
    .entries = {
        [KEY_1] = { '1', '!', 0, 0, 0, false },
        [KEY_2] = { '2', '@', 0, 0, 0, false },
        [KEY_3] = { '3', '#', 0, 0, 0, false },
        [KEY_4] = { '4', '$', 0, 0, 0, false },
        [KEY_5] = { '5', '%', 0, 0, 0, false },
        [KEY_6] = { '6', '^', 0, 0, 0, false },
        [KEY_7] = { '7', '&', 0, 0, 0, false },
        [KEY_8] = { '8', '*', 0, 0, 0, false },
        [KEY_9] = { '9', '(', 0, 0, 0, false },
        [KEY_0] = { '0', ')', 0, 0, 0, false },
        [KEY_MINUS] = { '[', '{', 0, 0, 0, false },
        [KEY_EQUAL] = { ']', '}', 0, 0, 0, false },

        [KEY_Q] = { '\'', '"', 0, 0, 0, false },
        [KEY_W] = { ',', '<', 0, 0, 0, false },
        [KEY_E] = { '.', '>', 0, 0, 0, false },
        [KEY_R] = { 'p', 'P', 0, 0, 0, true },
        [KEY_T] = { 'y', 'Y', 0, 0, 0, true },
        [KEY_Y] = { 'f', 'F', 0, 0, 0, true },
        [KEY_U] = { 'g', 'G', 0, 0, 0, true },
        [KEY_I] = { 'c', 'C', 0, 0, 0, true },
        [KEY_O] = { 'r', 'R', 0, 0, 0, true },
        [KEY_P] = { 'l', 'L', 0, 0, 0, true },
        [KEY_LBRACKET] = { '/', '?', 0, 0, 0, false },
        [KEY_RBRACKET] = { '=', '+', 0, 0, 0, false },

        [KEY_A] = { 'a', 'A', 0, 0, 0, true },
        [KEY_S] = { 'o', 'O', 0, 0, 0, true },
        [KEY_D] = { 'e', 'E', 0, 0, 0, true },
        [KEY_F] = { 'u', 'U', 0, 0, 0, true },
        [KEY_G] = { 'i', 'I', 0, 0, 0, true },
        [KEY_H] = { 'd', 'D', 0, 0, 0, true },
        [KEY_J] = { 'h', 'H', 0, 0, 0, true },
        [KEY_K] = { 't', 'T', 0, 0, 0, true },
        [KEY_L] = { 'n', 'N', 0, 0, 0, true },
        [KEY_SEMICOLON] = { 's', 'S', 0, 0, 0, true },
        [KEY_APOSTROPHE] = { '-', '_', 0, 0, 0, false },
        [KEY_GRAVE] = { '`', '~', 0, 0, 0, false },

        [KEY_BACKSLASH] = { '\\', '|', 0, 0, 0, false },
        [KEY_Z] = { ';', ':', 0, 0, 0, false },
        [KEY_X] = { 'q', 'Q', 0, 0, 0, true },
        [KEY_C] = { 'j', 'J', 0, 0, 0, true },
        [KEY_V] = { 'k', 'K', 0, 0, 0, true },
        [KEY_B] = { 'x', 'X', 0, 0, 0, true },
        [KEY_N] = { 'b', 'B', 0, 0, 0, true },
        [KEY_M] = { 'm', 'M', 0, 0, 0, true },
        [KEY_COMMA] = { 'w', 'W', 0, 0, 0, true },
        [KEY_DOT] = { 'v', 'V', 0, 0, 0, true },
        [KEY_SLASH] = { 'z', 'Z', 0, 0, 0, true },

        [KEY_SPACE] = { ' ', ' ', 0, 0, 0, false },
    }
};

static const keyboard_layout_t *g_layouts[] = {
    &layout_qwerty,
    &layout_azerty,
    &layout_qwertz,
    &layout_dvorak
};

static keymap_id_t g_current = KEYMAP_QWERTY;
static uint32_t g_active_dead = 0;

static const compose_entry_t g_compose_table[] = {
    { '^', 'a', 0x00E2 }, { '^', 'e', 0x00EA }, { '^', 'i', 0x00EE }, { '^', 'o', 0x00F4 }, { '^', 'u', 0x00FB },
    { '^', 'A', 0x00C2 }, { '^', 'E', 0x00CA }, { '^', 'I', 0x00CE }, { '^', 'O', 0x00D4 }, { '^', 'U', 0x00DB },

    { 0x00A8, 'a', 0x00E4 }, { 0x00A8, 'e', 0x00EB }, { 0x00A8, 'i', 0x00EF }, { 0x00A8, 'o', 0x00F6 }, { 0x00A8, 'u', 0x00FC }, { 0x00A8, 'y', 0x00FF },
    { 0x00A8, 'A', 0x00C4 }, { 0x00A8, 'E', 0x00CB }, { 0x00A8, 'I', 0x00CF }, { 0x00A8, 'O', 0x00D6 }, { 0x00A8, 'U', 0x00DC },

    { 0x00B4, 'a', 0x00E1 }, { 0x00B4, 'e', 0x00E9 }, { 0x00B4, 'i', 0x00ED }, { 0x00B4, 'o', 0x00F3 }, { 0x00B4, 'u', 0x00FA },
    { 0x00B4, 'A', 0x00C1 }, { 0x00B4, 'E', 0x00C9 }, { 0x00B4, 'I', 0x00CD }, { 0x00B4, 'O', 0x00D3 }, { 0x00B4, 'U', 0x00DA },

    { '`', 'a', 0x00E0 }, { '`', 'e', 0x00E8 }, { '`', 'i', 0x00EC }, { '`', 'o', 0x00F2 }, { '`', 'u', 0x00F9 },
    { 0, 0, 0 }
};

void keymap_init(void) {
    g_current = KEYMAP_QWERTY;
}

void keymap_set_current(keymap_id_t id) {
    if ((int)id < 0 || (int)id >= keymap_get_count()) return;
    g_current = id;
}

keymap_id_t keymap_get_current(void) {
    return g_current;
}

const char *keymap_get_name(keymap_id_t id) {
    if ((int)id < 0 || (int)id >= keymap_get_count()) return "Unknown";
    return g_layouts[id]->name;
}

int keymap_get_count(void) {
    return (int)(sizeof(g_layouts) / sizeof(g_layouts[0]));
}

static keymap_result_t make_result(uint32_t cp, bool dead) {
    keymap_result_t r;
    r.codepoint = cp;
    r.is_text = (cp != 0);
    r.is_dead = dead;
    return r;
}

keymap_result_t keymap_translate_keycode(uint16_t keycode, uint32_t mods) {
    if (keycode <= 0 || keycode >= KEY_MAX) 
        return make_result(0, false);

    const keymap_entry_t *e = &g_layouts[g_current]->entries[keycode];
    if (!e->normal && !e->shift && !e->altgr && !e->shift_altgr) 
        return make_result(0, false);

    bool shift = (mods & KB_MOD_SHIFT) != 0;
    bool caps  = (mods & KB_MOD_CAPS) != 0;
    bool altgr = (mods & KB_MOD_ALTGR) != 0;

    uint32_t cp = 0;
    uint8_t dead_mask_bit = 0;

    if (altgr) {
        if (shift) {
            cp = e->shift_altgr;
            dead_mask_bit = DEAD_SHIFT_ALTGR;
        } else {
            cp = e->altgr;
            dead_mask_bit = DEAD_ALTGR;
        }
    } else if (e->alpha) {
        bool uppercase = shift ^ caps;
        cp = uppercase ? e->shift : e->normal;
        dead_mask_bit = uppercase ? DEAD_SHIFT : DEAD_NORMAL;
    } else {
        cp = shift ? e->shift : e->normal;
        dead_mask_bit = shift ? DEAD_SHIFT : DEAD_NORMAL;
    }

    bool is_dead = (e->dead_mask & dead_mask_bit) != 0;

    if (is_dead && cp != 0) {
        g_active_dead = cp;
        return make_result(0, true);
    }

    if (g_active_dead && cp != 0) {
        uint32_t combined = keymap_compose(g_active_dead, cp);
        g_active_dead = 0;

        if (combined != 0) {
            return make_result(combined, false);
        }

        return make_result(cp, false);
    }

    return make_result(cp, false);
}

uint32_t keymap_compose(uint32_t dead_codepoint, uint32_t base_codepoint) {
    for (int i = 0; g_compose_table[i].dead != 0; i++) {
        if (g_compose_table[i].dead == dead_codepoint &&
            g_compose_table[i].base == base_codepoint) {
            return g_compose_table[i].result;
        }
    }
    return 0;
}

int keymap_legacy_key(uint16_t keycode, uint32_t codepoint) {
    if (codepoint != 0 && codepoint < 128) return (int)codepoint;

    switch (keycode) {
        case KEY_ESC:        return 27;
        case KEY_BACKSPACE:  return '\b';
        case KEY_TAB:        return '\t';
        case KEY_ENTER:
        case KEY_KP_ENTER:   return '\n';

        case KEY_ARROW_UP:    return 17;
        case KEY_ARROW_DOWN:  return 18;
        case KEY_ARROW_LEFT:  return 19;
        case KEY_ARROW_RIGHT: return 20;

        case KEY_LEFT_CTRL:
        case KEY_RIGHT_CTRL:  return 21;
        case KEY_LEFT_SHIFT:
        case KEY_RIGHT_SHIFT: return 24;
        case KEY_LEFT_ALT:    return 25;
        case KEY_RIGHT_ALT:   return 22;  // for compat w/ doom
        case KEY_CAPS_LOCK:   return 23;  // same here
        case KEY_DELETE:      return 127;

        case KEY_F1: return 141;
        case KEY_F2: return 142;
        case KEY_F3: return 143;
        case KEY_F4: return 144;
        case KEY_F5: return 145;
        case KEY_F6: return 146;
        case KEY_F7: return 147;
        case KEY_F8: return 148;
        case KEY_F9: return 149;
        case KEY_F10: return 150;
        case KEY_F11: return 151;
        case KEY_F12: return 152;

        default:              return 0;
    }
}