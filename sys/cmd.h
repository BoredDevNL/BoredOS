// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef CMD_H
#define CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cmd_init(void);
void cmd_reset(void);

void cmd_write(const char *str);
void cmd_write_len(const char *str, size_t len);
void cmd_putchar(char c);
void cmd_write_int(int n);
void cmd_write_hex(uint64_t n);
int cmd_get_cursor_col(void);
void cmd_screen_clear(void);

void cmd_increment_msg_count(void);
void cmd_reset_msg_count(void);

uint32_t cmd_get_config_value(const char *key);
void cmd_set_current_color(uint32_t color);
void cmd_set_raw_mode(bool enabled);
void cmd_process_finished(void);

#endif