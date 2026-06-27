// Copyright (c) 2026 Lluciocc (https://github.com/lluciocc)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef KEYCODES_H
#define KEYCODES_H

#include <stdint.h>

typedef enum {
    KEY_NONE = 0,

    KEY_ESC,

    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS,
    KEY_EQUAL,
    KEY_BACKSPACE,
    KEY_TAB,

    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LBRACKET,
    KEY_RBRACKET,
    KEY_ENTER,

    KEY_LEFT_CTRL,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON,
    KEY_APOSTROPHE,
    KEY_GRAVE,

    KEY_LEFT_SHIFT,
    KEY_BACKSLASH,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA,
    KEY_DOT,
    KEY_SLASH,
    KEY_RIGHT_SHIFT,

    KEY_KP_STAR,
    KEY_LEFT_ALT,
    KEY_SPACE,
    KEY_CAPS_LOCK,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUM_LOCK,
    KEY_SCROLL_LOCK,

    KEY_KP_7, KEY_KP_8, KEY_KP_9,
    KEY_KP_MINUS,
    KEY_KP_4, KEY_KP_5, KEY_KP_6,
    KEY_KP_PLUS,
    KEY_KP_1, KEY_KP_2, KEY_KP_3,
    KEY_KP_0,
    KEY_KP_DOT,

    KEY_F11,
    KEY_F12,

    KEY_KP_ENTER,
    KEY_RIGHT_CTRL,
    KEY_KP_SLASH,
    KEY_RIGHT_ALT,

    KEY_HOME,
    KEY_ARROW_UP,
    KEY_PAGE_UP,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    KEY_END,
    KEY_ARROW_DOWN,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_DELETE,

    KEY_MAX
} keycode_t;

#define KB_MOD_SHIFT   0x0001u
#define KB_MOD_CTRL    0x0002u
#define KB_MOD_ALT     0x0004u
#define KB_MOD_ALTGR   0x0008u
#define KB_MOD_CAPS    0x0010u
#define KB_MOD_NUM     0x0020u
#define KB_MOD_SCROLL  0x0040u

#endif