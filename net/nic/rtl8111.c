// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "rtl8111.h"
#include "io.h"
#include "kutils.h"
#include "platform.h"

#define RTL8111_MAC0          0x00
#define RTL8111_TDSAR         0x20
#define RTL8111_CR            0x37
#define RTL8111_TPPOLL        0x38
#define RTL8111_IMR           0x3C
#define RTL8111_ISR           0x3E
#define RTL8111_TCR           0x40
#define RTL8111_RCR           0x44
#define RTL8111_MULINT        0x5C
#define RTL8111_RMS           0xDA
#define RTL8111_MTPS          0xEC
#define RTL8111_RDSAR         0xE4

#define RTL8111_CR_TE         (1 << 2)
#define RTL8111_CR_RE         (1 << 3)
#define RTL8111_CR_RST        (1 << 4)

#define RTL8111_DESC_OWN      (1u << 31)
#define RTL8111_DESC_EOR      (1u << 30)
#define RTL8111_DESC_FS       (1u << 29)
#define RTL8111_DESC_LS       (1u << 28)

struct rtl8111_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t buf_addr;
} __attribute__((packed, aligned(16)));

#define RTL8111_NUM_RX_DESC   256
#define RTL8111_NUM_TX_DESC   256

static int rtl8111_initialized = 0;
static uint64_t mmio_base_addr = 0;
static uint8_t mac_addr[6];

static struct rtl8111_desc rx_desc[RTL8111_NUM_RX_DESC] __attribute__((aligned(256)));
static struct rtl8111_desc tx_desc[RTL8111_NUM_TX_DESC] __attribute__((aligned(256)));

static uint8_t rx_buffers[RTL8111_NUM_RX_DESC][2048] __attribute__((aligned(8)));
static uint8_t tx_buffers[RTL8111_NUM_TX_DESC][2048] __attribute__((aligned(8)));

static uint16_t rx_idx = 0;
static uint16_t tx_idx = 0;

static inline uint8_t  rtl8111_inb(uint16_t offset) { return *(volatile uint8_t*)(uintptr_t)(mmio_base_addr + offset); }
static inline uint16_t rtl8111_inw(uint16_t offset) { return *(volatile uint16_t*)(uintptr_t)(mmio_base_addr + offset); }
static inline uint32_t rtl8111_inl(uint16_t offset) { return *(volatile uint32_t*)(uintptr_t)(mmio_base_addr + offset); }

static inline void rtl8111_outb(uint16_t offset, uint8_t value) { *(volatile uint8_t*)(uintptr_t)(mmio_base_addr + offset) = value; }
static inline void rtl8111_outw(uint16_t offset, uint16_t value) { *(volatile uint16_t*)(uintptr_t)(mmio_base_addr + offset) = value; }
static inline void rtl8111_outl(uint16_t offset, uint32_t value) { *(volatile uint32_t*)(uintptr_t)(mmio_base_addr + offset) = value; }

int rtl8111_init(pci_device_t* pci_dev) {
    if (rtl8111_initialized) return 0;

    uint32_t command = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04);
    pci_write_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, command | (1 << 2) | (1 << 1));

    uint32_t bar2 = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x18); 

    uint64_t mmio_phys = 0;
    for (int bar_off = 0x10; bar_off <= 0x24; bar_off += 4) {
        uint32_t bar = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, bar_off);
        if (bar != 0 && bar != 0xFFFFFFFF && !(bar & 1)) {
            mmio_phys = (bar & ~0xF);
            if ((bar & 0x6) == 0x4) { 
                uint32_t bar_upper = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, bar_off + 4);
                mmio_phys |= ((uint64_t)bar_upper << 32);
            }
            break;
        }
    }

    if (mmio_phys == 0) return -1;

    mmio_base_addr = p2v(mmio_phys);

    extern void serial_write(const char *str);
    serial_write("[RTL8111] MMIO Base: 0x");
    char hex_buf[32]; itoa_hex(mmio_base_addr, hex_buf); serial_write(hex_buf); serial_write("\n");

    rtl8111_outb(RTL8111_CR, RTL8111_CR_RST);
    for (int i = 0; i < 100000; i++) {
        if (!(rtl8111_inb(RTL8111_CR) & RTL8111_CR_RST)) break;
    }

    uint32_t mac_low = rtl8111_inl(RTL8111_MAC0);
    uint32_t mac_high = rtl8111_inl(RTL8111_MAC0 + 4);
    mac_addr[0] = (mac_low >> 0) & 0xFF;
    mac_addr[1] = (mac_low >> 8) & 0xFF;
    mac_addr[2] = (mac_low >> 16) & 0xFF;
    mac_addr[3] = (mac_low >> 24) & 0xFF;
    mac_addr[4] = (mac_high >> 0) & 0xFF;
    mac_addr[5] = (mac_high >> 8) & 0xFF;

    serial_write("[RTL8111] MAC: ");
    for(int i=0; i<6; i++) {
        char buf[4]; itoa_hex(mac_addr[i], buf); serial_write(buf);
        if(i<5) serial_write(":");
    }
    serial_write("\n");

    rtl8111_outb(0x50, 0xC0); 

    memset(rx_desc, 0, sizeof(rx_desc));
    for (int i = 0; i < RTL8111_NUM_RX_DESC; i++) {
        uint64_t buf_phys = v2p((uint64_t)(uintptr_t)rx_buffers[i]);
        rx_desc[i].buf_addr = buf_phys;
        uint32_t cmd = 2048 | RTL8111_DESC_OWN;
        if (i == RTL8111_NUM_RX_DESC - 1) {
            cmd |= RTL8111_DESC_EOR;
        }
        rx_desc[i].opts1 = cmd;
        rx_desc[i].opts2 = 0;
    }
    uint64_t rx_ring_phys = v2p((uint64_t)(uintptr_t)rx_desc);
    rtl8111_outl(RTL8111_RDSAR, (uint32_t)rx_ring_phys);
    rtl8111_outl(RTL8111_RDSAR + 4, (uint32_t)(rx_ring_phys >> 32));

    memset(tx_desc, 0, sizeof(tx_desc));
    uint64_t tx_ring_phys = v2p((uint64_t)(uintptr_t)tx_desc);
    rtl8111_outl(RTL8111_TDSAR, (uint32_t)tx_ring_phys);
    rtl8111_outl(RTL8111_TDSAR + 4, (uint32_t)(tx_ring_phys >> 32));

    rtl8111_outl(RTL8111_RCR, 0x0F | (0x07 << 8) | (0x07 << 13));
    rtl8111_outl(RTL8111_TCR, (0x03 << 24)); 
    rtl8111_outw(RTL8111_RMS, 2048);
    rtl8111_outb(RTL8111_MTPS, 0x3F); 

    rtl8111_outb(0x50, 0x00);

    rtl8111_outb(RTL8111_CR, RTL8111_CR_RE | RTL8111_CR_TE);
    
    rtl8111_outw(RTL8111_MULINT, 0); 
    rtl8111_outw(RTL8111_IMR, 0x0005); 

    rx_idx = 0;
    tx_idx = 0;
    rtl8111_initialized = 1;
    return 0;
}

int rtl8111_send_packet(const void* data, size_t length) {
    if (!rtl8111_initialized) return -1;
    if (length > 2048) return -1;

    struct rtl8111_desc* desc = &tx_desc[tx_idx];
    
    if (desc->opts1 & RTL8111_DESC_OWN) {
        return -1; 
    }

    uint8_t* tx_buf = tx_buffers[tx_idx];
    uint8_t* src = (uint8_t*)data;
    for (size_t i = 0; i < length; i++) tx_buf[i] = src[i];

    desc->buf_addr = v2p((uint64_t)(uintptr_t)tx_buf);
    
    uint32_t cmd = length | RTL8111_DESC_OWN | RTL8111_DESC_FS | RTL8111_DESC_LS;
    if (tx_idx == RTL8111_NUM_TX_DESC - 1) {
        cmd |= RTL8111_DESC_EOR;
    }
    desc->opts2 = 0;
    __asm__ __volatile__ ("mfence");
    desc->opts1 = cmd;

    rtl8111_outb(RTL8111_TPPOLL, 0x40); 

    tx_idx = (tx_idx + 1) % RTL8111_NUM_TX_DESC;
    return 0;
}

int rtl8111_receive_packet(void* buffer, size_t buffer_size) {
    if (!rtl8111_initialized) return 0;

    uint16_t isr = rtl8111_inw(RTL8111_ISR);
    if (isr) {
        rtl8111_outw(RTL8111_ISR, isr);
    }

    struct rtl8111_desc* desc = &rx_desc[rx_idx];

    if (desc->opts1 & RTL8111_DESC_OWN) {
        return 0;
    }

    uint32_t len = desc->opts1 & 0x3FFF; 
    
    if (desc->opts1 & (1 << 21)) {
        len = 0;
    } else if (len > 0) {
        if (len > 4) len -= 4;
        if (len > buffer_size) len = buffer_size;
        
        uint8_t* dest = (uint8_t*)buffer;
        uint8_t* pkt = rx_buffers[rx_idx];
        for (uint32_t i = 0; i < len; i++) {
            dest[i] = pkt[i];
        }
    }

    uint32_t cmd = 2048 | RTL8111_DESC_OWN;
    if (rx_idx == RTL8111_NUM_RX_DESC - 1) {
        cmd |= RTL8111_DESC_EOR;
    }
    desc->buf_addr = v2p((uint64_t)(uintptr_t)rx_buffers[rx_idx]);
    desc->opts2 = 0;
    __asm__ __volatile__ ("mfence");
    desc->opts1 = cmd;

    rx_idx = (rx_idx + 1) % RTL8111_NUM_RX_DESC;
    return len;
}

int rtl8111_get_mac(uint8_t* mac_out) {
    if (!rtl8111_initialized) return -1;
    for (int i = 0; i < 6; i++) {
        mac_out[i] = mac_addr[i];
    }
    return 0;
}
