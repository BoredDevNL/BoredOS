// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "ps2.h"
#include "io.h"
#include "tty.h"
#include "process.h"
#include "kutils.h"

#include "network.h"
#include "lapic.h"
#include "smp.h"
#include <stdbool.h>
#include "keyboard.h"
#include "keymap.h"

extern void serial_write(const char *s);
extern void serial_write_num(uint32_t n);
extern void serial_print(const char *s);
extern void serial_print_hex(uint64_t n);

// --- Timer Handler ---
volatile uint64_t kernel_ticks = 0;

uint64_t timer_handler(registers_t *regs) {
    if (smp_this_cpu_id() == 0) {
        kernel_ticks++;
        network_process_frames();

        extern void k_beep_process(void);
        k_beep_process();
    }

    outb(0x20, 0x20);
    extern uint64_t process_schedule(uint64_t current_rsp);
    uint64_t new_rsp = process_schedule((uint64_t)regs);

    if (smp_cpu_count() > 1) {
        lapic_send_ipi_all();
    }

    return new_rsp;
}

// --- Keyboard ---
static void ps2_kbd_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout--) {
        if ((inb(0x64) & 2) == 0) return;
    }
}

static void ps2_update_leds(void) {
    uint8_t led_status = 0;
    uint32_t mods = keyboard_get_modifiers();

    if (mods & KB_MOD_CAPS)   led_status |= 4;
    if (mods & KB_MOD_NUM)    led_status |= 2;
    if (mods & KB_MOD_SCROLL) led_status |= 1;

    ps2_kbd_wait_write();
    outb(0x60, 0xED);
    ps2_kbd_wait_write();
    outb(0x60, led_status);
}

uint64_t keyboard_handler(registers_t *regs) {
    uint8_t scancode = inb(0x60);

    // Push raw scancode to active TTY (for /dev/keyboardX)
    tty_push_key(tty_get_active_id(), scancode);

    keyboard_event_t ev;
    if (keyboard_handle_set1_scancode(scancode, &ev)) {
        // Update LEDs if a lock key state changed
        if (ev.keycode == KEY_CAPS_LOCK || ev.keycode == KEY_NUM_LOCK || ev.keycode == KEY_SCROLL_LOCK) {
            if (ev.pressed) ps2_update_leds();
        }

        // Handle TTY Switching: Ctrl + Alt + F1-F10
        if ((ev.mods & KB_MOD_CTRL) && (ev.mods & KB_MOD_ALT) && ev.pressed) {
            if (ev.keycode >= KEY_F1 && ev.keycode <= KEY_F10) {
                int target = ev.keycode - KEY_F1;
                tty_switch(target);
                outb(0x20, 0x20); // EOI
                return (uint64_t)regs;
            }
        }

        // Push processed character to active TTY (for /dev/ttyX) only if text mode is active (not KD_GRAPHICS)
        if (ev.pressed && tty_get_blit_enabled()) {
            uint32_t cp = 0;
            if (ev.is_text) {
                cp = ev.codepoint;
            } else {
                int ch = keymap_legacy_key(ev.keycode, ev.codepoint);
                if (ch > 0 && ch < 128) {
                    cp = (uint32_t)ch;
                }
            }

            if (cp > 0) {
                int tty_id = tty_get_active_id();
                
                // Handle arrow keys and other special keys as ANSI escape sequences
                if (cp == 17) {  // UP arrow
                    tty_push_char(tty_id, 0x1B);
                    tty_push_char(tty_id, '[');
                    tty_push_char(tty_id, 'A');
                } else if (cp == 18) {  // DOWN arrow
                    tty_push_char(tty_id, 0x1B);
                    tty_push_char(tty_id, '[');
                    tty_push_char(tty_id, 'B');
                } else if (cp == 19) {  // LEFT arrow
                    tty_push_char(tty_id, 0x1B);
                    tty_push_char(tty_id, '[');
                    tty_push_char(tty_id, 'D');
                } else if (cp == 20) {  // RIGHT arrow
                    tty_push_char(tty_id, 0x1B);
                    tty_push_char(tty_id, '[');
                    tty_push_char(tty_id, 'C');
                } else if ((ev.mods & KB_MOD_CTRL) && !(ev.mods & KB_MOD_ALT)) {
                    // Special handling for Ctrl+C: send SIGINT to foreground process
                    if (cp == 'c' || cp == 'C') {
                        int fg_pid = tty_get_foreground(tty_id);
                        if (fg_pid > 0) {
                            extern process_t* process_get_by_pid(uint32_t pid);
                            process_t *proc = process_get_by_pid((uint32_t)fg_pid);
                            if (proc) {
                                proc->signal_pending |= (1ULL << 2); // SIGINT = 2
                            }
                        } else {
                            // No foreground process, send as input character
                            tty_push_char(tty_id, 0x03); // Ctrl+C = ETX
                        }
                    } else if (cp >= 'a' && cp <= 'z') {
                        tty_push_char(tty_id, (uint8_t)(cp - 0x60));
                    } else if (cp >= 'A' && cp <= 'Z') {
                        tty_push_char(tty_id, (uint8_t)(cp - 0x40)); 
                    } else if (cp == '[') {
                        tty_push_char(tty_id, 0x1B); // Ctrl+[ = ESC
                    } else if (cp == '\\') {
                        tty_push_char(tty_id, 0x1C); // Ctrl+\ = FS
                    } else if (cp == ']') {
                        tty_push_char(tty_id, 0x1D); // Ctrl+] = GS
                    } else if (cp == '^') {
                        tty_push_char(tty_id, 0x1E); // Ctrl+^ = RS
                    } else if (cp == '_') {
                        tty_push_char(tty_id, 0x1F); // Ctrl+_ = US
                    } else if (cp == '?') {
                        tty_push_char(tty_id, 0x7F); // Ctrl+? = DEL
                    }
                }
                // Handle Alt modifier: send ESC followed by the character
                else if (ev.mods & KB_MOD_ALT) {
                    tty_push_char(tty_id, 0x1B); // ESC
                    char utf8_buf[5];
                    int utf8_len = text_encode_utf8(cp, utf8_buf);
                    for (int i = 0; i < utf8_len; i++) {
                        tty_push_char(tty_id, (uint8_t)utf8_buf[i]);
                    }
                }
                // Normal character
                else {
                    char utf8_buf[5];
                    int utf8_len = text_encode_utf8(cp, utf8_buf);
                    for (int i = 0; i < utf8_len; i++) {
                        tty_push_char(tty_id, (uint8_t)utf8_buf[i]);
                    }
                }
            }
        }



    }

    outb(0x20, 0x20); // EOI
    return (uint64_t)regs;
}

// --- Mouse ---
static uint8_t mouse_cycle = 0;
static int8_t mouse_byte[4];
static bool mouse_has_wheel = true;

void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { // Write
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return;
        }
    } else { // Read
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return;
        }
    }
}

void mouse_write(uint8_t write) {
    mouse_wait(0);
    outb(0x64, 0xD4);
    mouse_wait(0);
    outb(0x60, write);
}

uint8_t mouse_read(void) {
    mouse_wait(1);
    return inb(0x60);
}

void mouse_init(void) {
    uint8_t status;
    int limit = 128;
    while (limit-- > 0 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL))
        (void)inb(PS2_DATA_PORT);

    // Enable Aux Device
    mouse_wait(0);
    outb(0x64, 0xA8);

    // Enable Interrupts
    mouse_wait(0);
    outb(0x64, 0x20);
    mouse_wait(1);
    status = inb(0x60) | 2;
    mouse_wait(0);
    outb(0x64, 0x60);
    mouse_wait(0);
    outb(0x60, status);

    // Set Defaults
    mouse_write(0xF6);
    mouse_read();

    // Enable Wheel
    mouse_write(0xF3); mouse_read(); mouse_write(200); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(100); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(80); mouse_read();

    mouse_write(0xF2);
    mouse_read();
    uint8_t id = mouse_read();
    (void)id;
    mouse_has_wheel = true;

    // Enable Streaming
    mouse_write(0xF4);
    mouse_read();
}

uint64_t mouse_handler(registers_t *regs) {
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) {
        outb(0x20, 0x20);
        outb(0xA0, 0x20);
        return (uint64_t)regs;
    }

    uint8_t b = inb(0x60);

    if (mouse_cycle == 0) {
        if ((b & 0xC8) != 0x08) {
            // Out of sync
        } else {
            mouse_byte[0] = b;
            mouse_cycle++;
        }
    } else if (mouse_cycle == 1) {
        mouse_byte[1] = b;
        mouse_cycle++;
    } else if (mouse_cycle == 2) {
        mouse_byte[2] = b;
        if (mouse_has_wheel) {
            mouse_cycle++;
        } else {
            mouse_cycle = 0;
            uint8_t packet[4];
            packet[0] = mouse_byte[0];
            packet[1] = mouse_byte[1];
            packet[2] = mouse_byte[2];
            packet[3] = 0;
            tty_push_mouse(tty_get_active_id(), packet, 4);
        }
    } else if (mouse_cycle == 3) {
        mouse_byte[3] = b;
        mouse_cycle = 0;
        uint8_t packet[4];
        packet[0] = mouse_byte[0];
        packet[1] = mouse_byte[1];
        packet[2] = mouse_byte[2];
        packet[3] = mouse_byte[3];
        tty_push_mouse(tty_get_active_id(), packet, 4);
    }



    outb(0x20, 0x20);
    outb(0xA0, 0x20);
    return (uint64_t)regs;
}

void ps2_init(void) {
    keymap_init();
    keyboard_init();
    mouse_init();
    ps2_update_leds();
}

bool ps2_shift_pressed(void) {
    return keyboard_shift_pressed();
}