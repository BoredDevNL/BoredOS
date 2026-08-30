// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "serial.h"
#include "io.h"
#include "spinlock.h"
#include "tty.h"
#include "kutils.h"
#include "slab.h"

static serial_ring_t g_com_rings[4];

static spinlock_t g_serial_lock = SPINLOCK_INIT;
static bool g_log_silenced = false;
static uint16_t g_debug_port = COM1_PORT;
static bool g_com2_present = false;

static serial_device_t g_early_devices[8];
static int g_early_device_count = 0;
static serial_device_t *g_serial_device_list_head = NULL;
static serial_device_t *g_serial_device_list_tail = NULL;
static int g_serial_device_count = 0;

static void isa_write_char(serial_device_t *dev, char c) {
    if (!dev || dev->io_port == 0) return;
    while ((inb(dev->io_port + 5) & 0x20) == 0);
    outb(dev->io_port, c);
}

static char isa_read_char(serial_device_t *dev) {
    if (!dev || dev->io_port == 0) return 0;
    while ((inb(dev->io_port + 5) & 0x01) == 0);
    return (char)inb(dev->io_port);
}
// for later use, eg USB serial devices or PCI serial devices
int serial_register_device(serial_device_t *dev) {
    if (!dev) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);

    int id = g_serial_device_count++;
    serial_device_t *node = (serial_device_t *)kmalloc(sizeof(serial_device_t));
    if (!node) {
        if (g_early_device_count < 8) {
            node = &g_early_devices[g_early_device_count++];
        } else {
            g_serial_device_count--;
            spinlock_release_irqrestore(&g_serial_lock, flags);
            return -1;
        }
    }

    *node = *dev;
    node->id = id;
    node->is_present = true;
    node->next = NULL;

    if (!node->write_char && node->type == SERIAL_TYPE_ISA) {
        node->write_char = isa_write_char;
    }
    if (!node->read_char && node->type == SERIAL_TYPE_ISA) {
        node->read_char = isa_read_char;
    }

    if (!g_serial_device_list_head) {
        g_serial_device_list_head = node;
        g_serial_device_list_tail = node;
    } else {
        g_serial_device_list_tail->next = node;
        g_serial_device_list_tail = node;
    }

    spinlock_release_irqrestore(&g_serial_lock, flags);
    return id;
}

serial_device_t* serial_get_device(int id) {
    if (id < 0) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    serial_device_t *curr = g_serial_device_list_head;
    while (curr) {
        if (curr->id == id) {
            spinlock_release_irqrestore(&g_serial_lock, flags);
            return curr;
        }
        curr = curr->next;
    }
    spinlock_release_irqrestore(&g_serial_lock, flags);
    return NULL;
}

int serial_get_device_count(void) {
    return g_serial_device_count;
}

void serial_device_write_char(serial_device_t *dev, char c) {
    if (!dev || !dev->is_present || !dev->write_char) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    dev->write_char(dev, c);
    spinlock_release_irqrestore(&g_serial_lock, flags);
}

void serial_device_write_str(serial_device_t *dev, const char *str) {
    if (!dev || !dev->is_present || !dev->write_char || !str) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    const char *p = str;
    while (*p) {
        dev->write_char(dev, *p++);
    }
    spinlock_release_irqrestore(&g_serial_lock, flags);
}

static void ring_push(serial_ring_t *ring, uint8_t val) {
    uint32_t next = (ring->head + 1) % SERIAL_RING_SIZE;
    if (next != ring->tail) {
        ring->buffer[ring->head] = val;
        ring->head = next;
    }
}

static int ring_pop(serial_ring_t *ring, char *buf, size_t len) {
    size_t count = 0;
    while (ring->head != ring->tail && count < len) {
        buf[count++] = (char)ring->buffer[ring->tail];
        ring->tail = (ring->tail + 1) % SERIAL_RING_SIZE;
    }
    return (int)count;
}

static serial_ring_t* get_ring_for_port(uint16_t port) {
    if (port == COM2_PORT) return &g_com_rings[1];
    if (port == COM3_PORT) return &g_com_rings[2];
    if (port == COM4_PORT) return &g_com_rings[3];
    return &g_com_rings[0];
}

void serial_init_port(uint16_t port) {
    outb(port + 1, 0x00); // Disable interrupts during config
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(port + 1, 0x00); //                  (hi byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(port + 1, 0x01); // Enable Received Data Available Interrupt
}

static bool probe_serial_port(uint16_t port) {
    outb(port + 7, 0xAE); // Write scratch byte
    if (inb(port + 7) != 0xAE) return false;
    outb(port + 7, 0x55);
    if (inb(port + 7) != 0x55) return false;
    return true;
}

static bool g_com1_present = false;
static bool g_com3_present = false;
static bool g_com4_present = false;

void serial_init(void) {
    uint16_t ports[4] = { COM1_PORT, COM2_PORT, COM3_PORT, COM4_PORT };
    uint8_t irqs[4] = { 4, 3, 4, 3 };

    for (int i = 0; i < 4; i++) {
        uint16_t port = ports[i];
        if (i == 0 || probe_serial_port(port)) {
            serial_init_port(port);
            serial_device_t dev;
            memset(&dev, 0, sizeof(dev));
            dev.type = SERIAL_TYPE_ISA;
            dev.io_port = port;
            dev.mmio_base = 0;
            dev.irq = irqs[i];
            dev.write_char = isa_write_char;
            dev.read_char = isa_read_char;
            int dev_id = serial_register_device(&dev);
            if (i == 0) g_com1_present = true;
            else if (i == 1 && dev_id >= 0) g_com2_present = true;
            else if (i == 2 && dev_id >= 0) g_com3_present = true;
            else if (i == 3 && dev_id >= 0) g_com4_present = true;
        }
    }

    if (g_com2_present) {
        g_debug_port = COM2_PORT;
    } else {
        g_debug_port = COM1_PORT;
    }
}

void serial_write_char(uint16_t port, char c) {
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    while ((inb(port + 5) & 0x20) == 0);
    outb(port, c);
    spinlock_release_irqrestore(&g_serial_lock, flags);
}

void serial_write_str(uint16_t port, const char *str) {
    if (!str) return;
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    const char *p = str;
    while (*p) {
        char c = *p++;
        while ((inb(port + 5) & 0x20) == 0);
        outb(port, c);
    }
    spinlock_release_irqrestore(&g_serial_lock, flags);
}

bool serial_has_received(uint16_t port) {
    return (inb(port + 5) & 0x01) != 0;
}

char serial_read_char(uint16_t port) {
    while (!serial_has_received(port));
    return (char)inb(port);
}

void serial_irq_handler(uint16_t port) {
    // Read IIR to acknowledge / check interrupt status
    uint8_t iir = inb(port + 2);
    if (iir & 1) return; // No interrupt pending

    serial_ring_t *ring = get_ring_for_port(port);

    int dev_index = 0;
    if (port == COM2_PORT) dev_index = 1;
    else if (port == COM3_PORT) dev_index = 2;
    else if (port == COM4_PORT) dev_index = 3;

    while (inb(port + 5) & 0x01) {
        uint8_t c = inb(port);
        ring_push(ring, c);

        // Push directly to corresponding TTY (ID 10 = ttyS0, ID 11 = ttyS1, etc.)
        int serial_tty_id = GRAPHICAL_TTY_COUNT + dev_index;
        extern void tty_push_serial_char(int tty_id, uint8_t ch);
        tty_push_serial_char(serial_tty_id, c);
    }
}

int serial_read_buf(uint16_t port, char *buf, size_t len) {
    uint64_t flags = spinlock_acquire_irqsave(&g_serial_lock);
    serial_ring_t *ring = get_ring_for_port(port);
    int ret = ring_pop(ring, buf, len);
    spinlock_release_irqrestore(&g_serial_lock, flags);
    return ret;
}

uint64_t serial_com1_handler(registers_t *regs) {
    if (g_com1_present) serial_irq_handler(COM1_PORT);
    if (g_com3_present) serial_irq_handler(COM3_PORT);
    outb(0x20, 0x20); // Master PIC EOI
    return (uint64_t)regs;
}

uint64_t serial_com2_handler(registers_t *regs) {
    if (g_com2_present) serial_irq_handler(COM2_PORT);
    if (g_com4_present) serial_irq_handler(COM4_PORT);
    outb(0x20, 0x20); // Master PIC EOI
    return (uint64_t)regs;
}

void serial_set_log_silenced(bool silenced) {
    g_log_silenced = silenced;
}

bool serial_is_log_silenced(void) {
    return g_log_silenced;
}

bool serial_is_com2_present(void) {
    return g_com2_present;
}

uint16_t serial_get_debug_port(void) {
    return g_debug_port;
}

void serial_set_debug_port(uint16_t port) {
    g_debug_port = port;
}
