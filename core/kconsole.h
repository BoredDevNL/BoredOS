#ifndef KCONSOLE_H
#define KCONSOLE_H

#include <stdint.h>
#include <stdbool.h>

void kconsole_init(void);
void kconsole_set_color(uint32_t color);
void kconsole_putc(char c);
void kconsole_write(const char *s);
void kconsole_set_active(bool active);

void serial_write(const char *str);
void serial_write_num_locked(uint32_t n);
void serial_write_num(uint32_t n);
void serial_write_hex_locked(uint64_t n);
void serial_write_hex(uint64_t n);

void log_ok(const char *msg);
void log_fail(const char *msg);

#endif // KCONSOLE_H
