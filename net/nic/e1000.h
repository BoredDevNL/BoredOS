// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID_82540EM 0x100E

#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EERD     0x0014
#define E1000_REG_ICR      0x00C0
#define E1000_REG_IMS      0x00D0
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_TIPG     0x0410
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818
#define E1000_REG_RAL      0x5400
#define E1000_REG_RAH      0x5404

#define E1000_CTRL_RST     (1 << 26)
#define E1000_CTRL_SLU     (1 << 6)

#define E1000_RCTL_EN      (1 << 1)
#define E1000_RCTL_SBP     (1 << 2)
#define E1000_RCTL_UPE     (1 << 3)
#define E1000_RCTL_MPE     (1 << 4)
#define E1000_RCTL_LPE     (1 << 5)
#define E1000_RCTL_LBM_NONE (0 << 6)
#define E1000_RCTL_RDMTS_HALF (0 << 8)
#define E1000_RCTL_MO_36   (0 << 12)
#define E1000_RCTL_BAM     (1 << 15)
#define E1000_RCTL_BSIZE_2048 (0 << 16)
#define E1000_RCTL_SECRC   (1 << 26)

#define E1000_TCTL_EN      (1 << 1)
#define E1000_TCTL_PSP     (1 << 3)
#define E1000_TCTL_CT      (0xFF << 4)
#define E1000_TCTL_COLD    (0x3FF << 12)

#define E1000_ICR_TXDW     (1 << 0)
#define E1000_ICR_RXT0     (1 << 7)

#define E1000_TX_RING_SIZE 256
#define E1000_RX_RING_SIZE 256

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    volatile uint32_t* mmio_base;
    pci_device_t pci_dev;
    int initialized;
    struct { uint8_t bytes[6]; } mac_address;
    e1000_tx_desc_t* tx_descriptors;
    void* tx_buffers[E1000_TX_RING_SIZE];
    uint16_t tx_head;
    uint16_t tx_tail;
    e1000_rx_desc_t* rx_descriptors;
    void* rx_buffers[E1000_RX_RING_SIZE];
    uint16_t rx_head;
    uint16_t rx_tail;
} e1000_device_t;

int e1000_init(pci_device_t* pci_dev);
static inline uint32_t e1000_read_reg(volatile uint32_t* mmio_base, uint16_t offset) { return mmio_base[offset / 4]; }
static inline void e1000_write_reg(volatile uint32_t* mmio_base, uint16_t offset, uint32_t value) { mmio_base[offset / 4] = value; }
e1000_device_t* e1000_get_device(void);
int e1000_send_packet(const void* data, size_t length);
int e1000_receive_packet(void* buffer, size_t buffer_size);

#endif
