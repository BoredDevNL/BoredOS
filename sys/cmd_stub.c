// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "cmd.h"
#include "kutils.h"

extern void serial_write(const char *str);

static void serial_write_char(char c) {
    char buf[2] = { c, 0 };
    serial_write(buf);
}

void cmd_init(void) {
}

void cmd_reset(void) {
}

void cmd_write(const char *str) {
    if (!str) return;
    serial_write(str);
}

void cmd_write_len(const char *str, size_t len) {
    if (!str || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        serial_write_char(str[i]);
    }
}

void cmd_putchar(char c) {
    serial_write_char(c);
}

void cmd_write_int(int n) {
    char buf[32];
    itoa(n, buf);
    cmd_write(buf);
}

void cmd_write_hex(uint64_t n) {
    char buf[17];
    itoa_hex(n, buf);
    cmd_write("0x");
    cmd_write(buf);
}

int cmd_get_cursor_col(void) {
    return 0;
}

void cmd_screen_clear(void) {
}

void cmd_increment_msg_count(void) {
}

void cmd_reset_msg_count(void) {
}

uint32_t cmd_get_config_value(const char *key) {
    (void)key;
    return 0;
}

void cmd_set_current_color(uint32_t color) {
    (void)color;
}

void cmd_set_raw_mode(bool enabled) {
    (void)enabled;
}

void cmd_process_finished(void) {
}
