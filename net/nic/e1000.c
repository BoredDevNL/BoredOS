// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include <stdint.h>
#include <stddef.h>
#include "e1000.h"
#include "pci.h"
#include "io.h"
#include "platform.h"
#include "kutils.h"

static e1000_device_t e1000_dev;
static int e1000_initialized = 0;
static e1000_tx_desc_t tx_descriptors[E1000_TX_RING_SIZE] __attribute__((aligned(16)));
static e1000_rx_desc_t rx_descriptors[E1000_RX_RING_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[E1000_TX_RING_SIZE][2048] __attribute__((aligned(16)));
static uint8_t rx_buffers[E1000_RX_RING_SIZE][2048] __attribute__((aligned(16)));

static void* kmemcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

int e1000_init(pci_device_t* pci_dev) {
    if (e1000_initialized) return 0;
    uint32_t bar0 = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x10);
    if (bar0 == 0 || bar0 == 0xFFFFFFFF) return -1;
    if (bar0 & 1) return -1;
    uint64_t mmio_base_phys = (uint64_t)(bar0 & ~0xF);
    volatile uint32_t* mmio_base = (volatile uint32_t*)(uintptr_t)p2v(mmio_base_phys);
    e1000_dev.mmio_base = mmio_base;
    e1000_dev.pci_dev = *pci_dev;
    e1000_dev.initialized = 0;

    extern void serial_write(const char *str);
    serial_write("[E1000] MMIO Base (virt): 0x");
    char hex_buf[32];
    itoa_hex((uint64_t)mmio_base, hex_buf);
    serial_write(hex_buf);
    serial_write("\n");

    uint32_t status_reg = e1000_read_reg(mmio_base, E1000_REG_STATUS);
    serial_write("[E1000] Status: 0x");
    itoa_hex(status_reg, hex_buf);
    serial_write(hex_buf);
    serial_write("\n");

    uint32_t command = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04);
    command |= (1 << 2);
    command |= (1 << 1);
    pci_write_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, command);

    uint32_t ctrl = e1000_read_reg(mmio_base, E1000_REG_CTRL);
    e1000_write_reg(mmio_base, E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    for (int i = 0; i < 100000; i++) {
        ctrl = e1000_read_reg(mmio_base, E1000_REG_CTRL);
        if (!(ctrl & E1000_CTRL_RST)) break;
    }

    uint32_t ral = e1000_read_reg(mmio_base, E1000_REG_RAL);
    uint32_t rah = e1000_read_reg(mmio_base, E1000_REG_RAH);
    uint16_t* mac_16 = (uint16_t*)&e1000_dev.mac_address;
    mac_16[0] = (uint16_t)(ral & 0xFFFF);
    mac_16[1] = (uint16_t)(ral >> 16);
    mac_16[2] = (uint16_t)(rah & 0xFFFF);

    serial_write("[E1000] MAC: ");
    for(int i=0; i<6; i++) {
        char buf[4];
        itoa_hex(e1000_dev.mac_address.bytes[i], buf);
        serial_write(buf);
        if(i<5) serial_write(":");
    }
    serial_write("\n");

    e1000_write_reg(mmio_base, E1000_REG_RAL, ral);
    e1000_write_reg(mmio_base, E1000_REG_RAH, rah | (1u << 31));

    e1000_dev.tx_descriptors = tx_descriptors;
    e1000_dev.tx_head = 0;
    e1000_dev.tx_tail = 0;
    memset(tx_descriptors, 0, sizeof(tx_descriptors));
    memset(tx_buffers, 0, sizeof(tx_buffers));

    for (int i = 0; i < E1000_TX_RING_SIZE; i++) {
        e1000_dev.tx_buffers[i] = tx_buffers[i];
        e1000_dev.tx_descriptors[i].buffer_addr = v2p((uint64_t)(uintptr_t)tx_buffers[i]);
    }
    uint64_t tx_desc_phys = v2p((uint64_t)(uintptr_t)tx_descriptors);
    e1000_write_reg(mmio_base, E1000_REG_TDBAL, (uint32_t)(tx_desc_phys & 0xFFFFFFFF));
    e1000_write_reg(mmio_base, E1000_REG_TDBAH, (uint32_t)(tx_desc_phys >> 32));
    e1000_write_reg(mmio_base, E1000_REG_TDLEN, E1000_TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    e1000_write_reg(mmio_base, E1000_REG_TDH, 0);
    e1000_write_reg(mmio_base, E1000_REG_TDT, 0);
    
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 4) | (0x40 << 12);
    e1000_write_reg(mmio_base, E1000_REG_TCTL, tctl);
    e1000_write_reg(mmio_base, E1000_REG_TIPG, 0x0060200A);

    e1000_dev.rx_descriptors = rx_descriptors;
    e1000_dev.rx_head = 0;
    e1000_dev.rx_tail = E1000_RX_RING_SIZE - 1;
    memset(rx_descriptors, 0, sizeof(rx_descriptors));
    memset(rx_buffers, 0, sizeof(rx_buffers));
    for (int i = 0; i < E1000_RX_RING_SIZE; i++) {
        e1000_dev.rx_buffers[i] = rx_buffers[i];
        e1000_dev.rx_descriptors[i].buffer_addr = v2p((uint64_t)(uintptr_t)rx_buffers[i]);
    }
    uint64_t rx_desc_phys = v2p((uint64_t)(uintptr_t)rx_descriptors);
    e1000_write_reg(mmio_base, E1000_REG_RDBAL, (uint32_t)(rx_desc_phys & 0xFFFFFFFF));
    e1000_write_reg(mmio_base, E1000_REG_RDBAH, (uint32_t)(rx_desc_phys >> 32));
    e1000_write_reg(mmio_base, E1000_REG_RDLEN, E1000_RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    e1000_write_reg(mmio_base, E1000_REG_RDH, 0);
    e1000_write_reg(mmio_base, E1000_REG_RDT, E1000_RX_RING_SIZE - 1);
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE |
                    E1000_RCTL_LPE | E1000_RCTL_LBM_NONE | E1000_RCTL_RDMTS_HALF |
                    E1000_RCTL_MO_36 | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write_reg(mmio_base, E1000_REG_RCTL, rctl);
    ctrl = e1000_read_reg(mmio_base, E1000_REG_CTRL);
    e1000_write_reg(mmio_base, E1000_REG_CTRL, ctrl | E1000_CTRL_SLU);
    e1000_dev.initialized = 1;
    e1000_initialized = 1;
    return 0;
}

e1000_device_t* e1000_get_device(void) {
    if (!e1000_initialized) return NULL;
    return &e1000_dev;
}

int e1000_get_mac(uint8_t* mac_out) {
    if (!e1000_initialized) return -1;
    for (int i = 0; i < 6; i++) {
        mac_out[i] = e1000_dev.mac_address.bytes[i];
    }
    return 0;
}

int e1000_send_packet(const void* data, size_t length) {
    if (!e1000_initialized || !e1000_dev.initialized) return -1;
    if (length > 2048) return -1;
    
    volatile uint32_t* mmio = e1000_dev.mmio_base;
    uint16_t tail = e1000_dev.tx_tail;
    uint16_t next_tail = (tail + 1) % E1000_TX_RING_SIZE;
    
    if (e1000_dev.tx_descriptors[tail].cmd != 0) {
        for(int i=0; i<1000000; i++) {
            if (e1000_dev.tx_descriptors[tail].status & 0x01) break;
            __asm__ __volatile__("pause");
        }
    }

    kmemcpy(e1000_dev.tx_buffers[tail], data, length);
    e1000_dev.tx_descriptors[tail].length = (uint16_t)length;
    e1000_dev.tx_descriptors[tail].cmd = 0x0B;
    e1000_dev.tx_descriptors[tail].status = 0;
    
    e1000_dev.tx_tail = next_tail;
    e1000_write_reg(mmio, E1000_REG_TDT, e1000_dev.tx_tail);
    
    return 0;
}

int e1000_receive_packet(void* buffer, size_t buffer_size) {
    if (!e1000_initialized || !e1000_dev.initialized) return 0;
    uint16_t next_idx = (e1000_dev.rx_tail + 1) % E1000_RX_RING_SIZE;
    
    if (!(e1000_dev.rx_descriptors[next_idx].status & 1)) return 0;
    
    uint16_t length = e1000_dev.rx_descriptors[next_idx].length;
    // Do NOT subtract 4. SECRC strips the CRC and the length already reflects this.
    
    if (length > buffer_size) length = (uint16_t)buffer_size;
    kmemcpy(buffer, e1000_dev.rx_buffers[next_idx], length);
    
    e1000_dev.rx_descriptors[next_idx].status = 0;
    e1000_dev.rx_descriptors[next_idx].length = 0;
    
    e1000_dev.rx_tail = next_idx;
    e1000_write_reg(e1000_dev.mmio_base, E1000_REG_RDT, e1000_dev.rx_tail);
    
    return (int)length;
}
