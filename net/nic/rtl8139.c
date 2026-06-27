// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "rtl8139.h"
#include "io.h"
#include "kutils.h"
#include "platform.h"

#define RTL8139_MAC_0         0x00
#define RTL8139_TSD0          0x10
#define RTL8139_TSAD0         0x20
#define RTL8139_RBSTART       0x30
#define RTL8139_CR            0x37
#define RTL8139_CAPR          0x38
#define RTL8139_CBR           0x3A
#define RTL8139_IMR           0x3C
#define RTL8139_ISR           0x3E
#define RTL8139_TCR           0x40
#define RTL8139_RCR           0x44
#define RTL8139_CONFIG_1      0x52

#define RTL8139_RCR_AAP       (1 << 0) // Accept All Packets
#define RTL8139_RCR_APM       (1 << 1) // Accept Physical Match Packets
#define RTL8139_RCR_AM        (1 << 2) // Accept Multicast Packets
#define RTL8139_RCR_AB        (1 << 3) // Accept Broadcast Packets
#define RTL8139_RCR_WRAP      (1 << 7) // WRAP

#define RTL8139_CR_TE         (1 << 2) // Transmitter Enable
#define RTL8139_CR_RE         (1 << 3) // Receiver Enable
#define RTL8139_CR_RST        (1 << 4) // Reset

static int rtl8139_initialized = 0;
static uint64_t mmio_base_addr = 0;
static uint8_t mac_addr[6];

// Receive buffer must be 8K + 16 bytes + 1.5K
// Let's use 32K buffer for safety, which is standard.
static uint8_t rx_buffer[32768 + 16 + 1500] __attribute__((aligned(4096)));
static uint16_t rx_ptr = 0; // Current read position

// Transmit buffers (4 descriptors in RTL8139)
static uint8_t tx_buffers[4][4096] __attribute__((aligned(4096)));
static uint8_t tx_curr = 0;

static inline uint8_t rtl8139_inb(uint16_t offset) { return *(volatile uint8_t*)(uintptr_t)(mmio_base_addr + offset); }
static inline uint16_t rtl8139_inw(uint16_t offset) { return *(volatile uint16_t*)(uintptr_t)(mmio_base_addr + offset); }
static inline uint32_t rtl8139_inl(uint16_t offset) { return *(volatile uint32_t*)(uintptr_t)(mmio_base_addr + offset); }

static inline void rtl8139_outb(uint16_t offset, uint8_t value) { *(volatile uint8_t*)(uintptr_t)(mmio_base_addr + offset) = value; }
static inline void rtl8139_outw(uint16_t offset, uint16_t value) { *(volatile uint16_t*)(uintptr_t)(mmio_base_addr + offset) = value; }
static inline void rtl8139_outl(uint16_t offset, uint32_t value) { *(volatile uint32_t*)(uintptr_t)(mmio_base_addr + offset) = value; }

int rtl8139_init(pci_device_t* pci_dev) {
    if (rtl8139_initialized) return 0;

    // Enable bus mastering
    uint32_t command = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04);
    pci_write_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x04, command | (1 << 2) | (1 << 1));

    uint32_t bar1 = pci_read_config(pci_dev->bus, pci_dev->device, pci_dev->function, 0x14); // BAR1: MMIO
    if (bar1 == 0 || bar1 == 0xFFFFFFFF) return -1;
    if (bar1 & 1) return -1; // Should not be I/O space

    mmio_base_addr = p2v(bar1 & ~0xF);

    extern void serial_write(const char *str);
    serial_write("[RTL8139] MMIO Base: 0x");
    char hex_buf[32]; itoa_hex(mmio_base_addr, hex_buf); serial_write(hex_buf); serial_write("\n");

    // Power on (CONFIG1)
    rtl8139_outb(RTL8139_CONFIG_1, 0x00);

    // Software Reset
    rtl8139_outb(RTL8139_CR, RTL8139_CR_RST);
    while (rtl8139_inb(RTL8139_CR) & RTL8139_CR_RST) {
        // Wait
    }

    // Read MAC
    uint32_t mac_low = rtl8139_inl(RTL8139_MAC_0);
    uint32_t mac_high = rtl8139_inw(RTL8139_MAC_0 + 4);
    mac_addr[0] = (mac_low >> 0) & 0xFF;
    mac_addr[1] = (mac_low >> 8) & 0xFF;
    mac_addr[2] = (mac_low >> 16) & 0xFF;
    mac_addr[3] = (mac_low >> 24) & 0xFF;
    mac_addr[4] = (mac_high >> 0) & 0xFF;
    mac_addr[5] = (mac_high >> 8) & 0xFF;

    serial_write("[RTL8139] MAC: ");
    for(int i=0; i<6; i++) {
        char buf[4];
        itoa_hex(mac_addr[i], buf);
        serial_write(buf);
        if(i<5) serial_write(":");
    }
    serial_write("\n");

    // Init RX buffer
    uint32_t rx_phys = v2p((uint64_t)(uintptr_t)rx_buffer);
    rtl8139_outl(RTL8139_RBSTART, rx_phys);

    // Set IMR / ISR
    rtl8139_outw(RTL8139_IMR, 0x0005); // TOK and ROK

    // Set RCR (Receive Configuration Register)
    // Accept Broadcast/Multicast/Physical Match + WRAP
    rtl8139_outl(RTL8139_RCR, RTL8139_RCR_AB | RTL8139_RCR_AM | RTL8139_RCR_APM | RTL8139_RCR_WRAP | (3 << 11)); // 32k rx buffer

    // Enable Transmitter and Receiver
    rtl8139_outb(RTL8139_CR, RTL8139_CR_RE | RTL8139_CR_TE);
    
    // Config TCR
    rtl8139_outl(RTL8139_TCR, (0x03 << 24)); // IFG

    rtl8139_initialized = 1;
    return 0;
}

int rtl8139_send_packet(const void* data, size_t length) {
    if (!rtl8139_initialized) return -1;
    if (length > 1792) return -1;

    // Use current tx buffer
    uint8_t* tx_buf = tx_buffers[tx_curr];
    uint8_t* src = (uint8_t*)data;
    for (size_t i = 0; i < length; i++) tx_buf[i] = src[i];

    uint32_t phys = v2p((uint64_t)(uintptr_t)tx_buf);
    rtl8139_outl(RTL8139_TSAD0 + (tx_curr * 4), phys);
    
    // Status is length | clear bit 13 to send
    rtl8139_outl(RTL8139_TSD0 + (tx_curr * 4), length);

    tx_curr = (tx_curr + 1) % 4;
    return 0;
}

int rtl8139_receive_packet(void* buffer, size_t buffer_size) {
    if (!rtl8139_initialized) return 0;

    uint8_t cmd = rtl8139_inb(RTL8139_CR);
    if ((cmd & 1) == 1) { // Buffer empty
        return 0; 
    }

    uint16_t* rx_head = (uint16_t*)(rx_buffer + rx_ptr);
    uint16_t rx_status = rx_head[0];
    uint16_t rx_length = rx_head[1]; // includes 4 bytes CRC at end

    if (rx_status & (1 << 5)) { // Bad packet
        // Bad packet received. We need to skip it or reset.
    }

    if (rx_length > 0 && rx_length <= buffer_size + 4) {
        uint8_t* pkt = (uint8_t*)(rx_head) + 4;
        uint16_t net_len = rx_length - 4; // Strip CRC
        
        uint8_t* dest = (uint8_t*)buffer;
        for (int i = 0; i < net_len; i++) {
            dest[i] = pkt[i];
        }

        // Update rx_ptr
        rx_ptr = (rx_ptr + rx_length + 4 + 3) & ~3; // Align up to 4 bytes
        if (rx_ptr > 32768) {
            rx_ptr -= 32768; // Wrap around
        }

        rtl8139_outw(RTL8139_CAPR, rx_ptr - 16);
        return net_len;
    }

    // Default error handling, skip it
    rx_ptr = (rx_ptr + rx_length + 4 + 3) & ~3;
    if (rx_ptr > 32768) rx_ptr -= 32768;
    rtl8139_outw(RTL8139_CAPR, rx_ptr - 16);
    return 0;
}

int rtl8139_get_mac(uint8_t* mac_out) {
    if (!rtl8139_initialized) return -1;
    for (int i = 0; i < 6; i++) {
        mac_out[i] = mac_addr[i];
    }
    return 0;
}
