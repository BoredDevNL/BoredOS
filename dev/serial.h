// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8
#define COM3_PORT 0x3E8
#define COM4_PORT 0x2E8

#define SERIAL_RING_SIZE 1024

typedef enum {
    SERIAL_TYPE_ISA,
    SERIAL_TYPE_PCI_UART,
    SERIAL_TYPE_USB_ACM
} serial_type_t;

struct serial_device;

typedef struct serial_device {
    int id;                      // 0 for ttyS0, 1 for ttyS1, etc.
    serial_type_t type;
    uint16_t io_port;            // Legacy I/O port (for ISA)
    uintptr_t mmio_base;         // MMIO base address (for PCI / USB)
    uint8_t irq;
    bool is_present;
    
    void (*write_char)(struct serial_device *dev, char c);
    char (*read_char)(struct serial_device *dev);
    
    struct serial_device *next;
} serial_device_t;

typedef struct {
    uint8_t buffer[SERIAL_RING_SIZE];
    uint32_t head;
    uint32_t tail;
} serial_ring_t;

typedef struct registers_t registers_t;
uint64_t serial_com1_handler(registers_t *regs);
uint64_t serial_com2_handler(registers_t *regs);

void serial_init(void);
void serial_init_port(uint16_t port);

int serial_register_device(serial_device_t *dev);
serial_device_t* serial_get_device(int id);
int serial_get_device_count(void);

void serial_device_write_char(serial_device_t *dev, char c);
void serial_device_write_str(serial_device_t *dev, const char *str);

void serial_write_char(uint16_t port, char c);
void serial_write_str(uint16_t port, const char *str);
bool serial_has_received(uint16_t port);
char serial_read_char(uint16_t port);

void serial_irq_handler(uint16_t port);
int serial_read_buf(uint16_t port, char *buf, size_t len);

void serial_set_log_silenced(bool silenced);
bool serial_is_log_silenced(void);
bool serial_is_com2_present(void);

uint16_t serial_get_debug_port(void);
void serial_set_debug_port(uint16_t port);

#endif
