// Copyright (c) 2026 Lluciocc (https://github.com/lluciocc)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "keyboard.h"
#include "keymap.h"
#include "io.h"
#include "ps2.h"

typedef struct {
    bool e0_prefix;
    bool left_shift;
    bool right_shift;
    bool left_ctrl;
    bool right_ctrl;
    bool left_alt;
    bool right_alt;
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    uint32_t dead_key;
} keyboard_state_t;

static keyboard_state_t g_kb;

// table of scancode to keycode for set 1 (without E0 prefix)
static const uint16_t set1_base[128] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1, [0x03] = KEY_2, [0x04] = KEY_3, [0x05] = KEY_4,
    [0x06] = KEY_5, [0x07] = KEY_6, [0x08] = KEY_7, [0x09] = KEY_8,
    [0x0A] = KEY_9, [0x0B] = KEY_0,
    [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUAL,
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,

    [0x10] = KEY_Q, [0x11] = KEY_W, [0x12] = KEY_E, [0x13] = KEY_R,
    [0x14] = KEY_T, [0x15] = KEY_Y, [0x16] = KEY_U, [0x17] = KEY_I,
    [0x18] = KEY_O, [0x19] = KEY_P,
    [0x1A] = KEY_LBRACKET, [0x1B] = KEY_RBRACKET,
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LEFT_CTRL,

    [0x1E] = KEY_A, [0x1F] = KEY_S, [0x20] = KEY_D, [0x21] = KEY_F,
    [0x22] = KEY_G, [0x23] = KEY_H, [0x24] = KEY_J, [0x25] = KEY_K,
    [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON, [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,

    [0x2A] = KEY_LEFT_SHIFT,
    [0x2B] = KEY_BACKSLASH,
    [0x2C] = KEY_Z, [0x2D] = KEY_X, [0x2E] = KEY_C, [0x2F] = KEY_V,
    [0x30] = KEY_B, [0x31] = KEY_N, [0x32] = KEY_M,
    [0x33] = KEY_COMMA, [0x34] = KEY_DOT, [0x35] = KEY_SLASH,
    [0x36] = KEY_RIGHT_SHIFT,

    [0x37] = KEY_KP_STAR,
    [0x38] = KEY_LEFT_ALT,
    [0x39] = KEY_SPACE,
    [0x3A] = KEY_CAPS_LOCK,

    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUM_LOCK,
    [0x46] = KEY_SCROLL_LOCK,

    [0x47] = KEY_KP_7, [0x48] = KEY_KP_8, [0x49] = KEY_KP_9,
    [0x4A] = KEY_KP_MINUS,
    [0x4B] = KEY_KP_4, [0x4C] = KEY_KP_5, [0x4D] = KEY_KP_6,
    [0x4E] = KEY_KP_PLUS,
    [0x4F] = KEY_KP_1, [0x50] = KEY_KP_2, [0x51] = KEY_KP_3,
    [0x52] = KEY_KP_0, [0x53] = KEY_KP_DOT,

    [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

// table of scancode to keycode for set 1 with E0 prefix
static const uint16_t set1_ext[128] = {
    [0x1C] = KEY_KP_ENTER,
    [0x1D] = KEY_RIGHT_CTRL,
    [0x35] = KEY_KP_SLASH,
    [0x38] = KEY_RIGHT_ALT,

    [0x47] = KEY_HOME,
    [0x48] = KEY_ARROW_UP,
    [0x49] = KEY_PAGE_UP,
    [0x4B] = KEY_ARROW_LEFT,
    [0x4D] = KEY_ARROW_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_ARROW_DOWN,
    [0x51] = KEY_PAGE_DOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
};

void keyboard_init(void) {
    /*
        Flush all data in the controller output buffer
        this can cause the PS/2 Keyboard device to not work
        on real hardware specifically
        (even when Legacy USB keyboard Emulation is emulating the PS/2 keyboard)
        Myles
    */
    int limit = 128;
    while (limit-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL))
        (void)inb(KBD_DATA_PORT);

    g_kb.e0_prefix = false;
    g_kb.left_shift = false;
    g_kb.right_shift = false;
    g_kb.left_ctrl = false;
    g_kb.right_ctrl = false;
    g_kb.left_alt = false;
    g_kb.right_alt = false;
    g_kb.caps_lock = false;
    g_kb.num_lock = false;
    g_kb.scroll_lock = false;
    g_kb.dead_key = 0;
}

// Convert a set 1 scancode (with optional E0 prefix) to a keycode. Returns KEY_NONE if the scancode is invalid or unmapped.
uint16_t keyboard_keycode_from_set1(uint8_t scancode, bool extended) {
    if (scancode >= 128) return KEY_NONE;
    return extended ? set1_ext[scancode] : set1_base[scancode];
}

// Update the state of modifier keys based on the keycode and press/release event.
static void update_mod_state(uint16_t keycode, bool pressed) {
    switch (keycode) {
        case KEY_LEFT_SHIFT:  g_kb.left_shift  = pressed; break;
        case KEY_RIGHT_SHIFT: g_kb.right_shift = pressed; break;
        case KEY_LEFT_CTRL:   g_kb.left_ctrl   = pressed; break;
        case KEY_RIGHT_CTRL:  g_kb.right_ctrl  = pressed; break;
        case KEY_LEFT_ALT:    g_kb.left_alt    = pressed; break;
        case KEY_RIGHT_ALT:   g_kb.right_alt   = pressed; break;

        case KEY_CAPS_LOCK:
            if (pressed) g_kb.caps_lock = !g_kb.caps_lock;
            break;
        case KEY_NUM_LOCK:
            if (pressed) g_kb.num_lock = !g_kb.num_lock;
            break;
        case KEY_SCROLL_LOCK:
            if (pressed) g_kb.scroll_lock = !g_kb.scroll_lock;
            break;
        default:
            break;
    }
}

// Get the current state of modifier keys as a bitmask.
uint32_t keyboard_get_modifiers(void) {
    uint32_t mods = 0;
    if (g_kb.left_shift || g_kb.right_shift) mods |= KB_MOD_SHIFT;
    if (g_kb.left_ctrl || g_kb.right_ctrl)   mods |= KB_MOD_CTRL;
    if (g_kb.left_alt)                       mods |= KB_MOD_ALT;
    if (g_kb.right_alt)                      mods |= KB_MOD_ALTGR;
    if (g_kb.caps_lock)                      mods |= KB_MOD_CAPS;
    if (g_kb.num_lock)                       mods |= KB_MOD_NUM;
    if (g_kb.scroll_lock)                    mods |= KB_MOD_SCROLL;
    return mods;
}

// Helper functions to check specific modifiers
bool keyboard_shift_pressed(void) {
    return (keyboard_get_modifiers() & KB_MOD_SHIFT) != 0;
}

bool keyboard_ctrl_pressed(void) {
    return (keyboard_get_modifiers() & KB_MOD_CTRL) != 0;
}
 
bool keyboard_handle_set1_scancode(uint8_t scancode, keyboard_event_t *ev) {
    if (!ev) return false;

    ev->keycode = KEY_NONE;
    ev->codepoint = 0;
    ev->mods = keyboard_get_modifiers();
    ev->pressed = false;
    ev->repeat = false;
    ev->is_text = false;

    if (scancode == 0xE0) {
        g_kb.e0_prefix = true;
        return false;
    }

    if (scancode == 0xE1) {
        g_kb.e0_prefix = false;
        return false; // ignore Pause/Break's multi-byte sequence for simplicity
    }

    bool release = (scancode & 0x80) != 0;
    uint8_t base = (uint8_t)(scancode & 0x7F);

    uint16_t keycode = keyboard_keycode_from_set1(base, g_kb.e0_prefix);
    g_kb.e0_prefix = false;

    if (keycode == KEY_NONE) return false;

    bool pressed = !release;
    update_mod_state(keycode, pressed);

    ev->keycode = keycode;
    ev->pressed = pressed;
    ev->mods = keyboard_get_modifiers();

    if (!pressed) {
        return true;
    }

    keymap_result_t r = keymap_translate_keycode(keycode, ev->mods);

    if (r.is_dead) {
        g_kb.dead_key = r.codepoint;
        return true;
    }

    if (r.is_text) {
        uint32_t cp = r.codepoint;
        if (g_kb.dead_key != 0) {
            uint32_t composed = keymap_compose(g_kb.dead_key, cp);
            if (composed != 0) cp = composed;
            g_kb.dead_key = 0;
        }
        ev->codepoint = cp;
        ev->is_text = true;
    }

    return true;
}