// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "nic.h"
#include "pci.h"
#include "kutils.h"

extern int e1000_init(pci_device_t* pci_dev);
extern int rtl8139_init(pci_device_t* pci_dev);
extern int virtio_net_init(pci_device_t* pci_dev);

extern int e1000_send_packet(const void* data, size_t length);
extern int e1000_receive_packet(void* buffer, size_t buffer_size);
extern int e1000_get_mac(uint8_t* mac_out);

extern int rtl8139_send_packet(const void* data, size_t length);
extern int rtl8139_receive_packet(void* buffer, size_t buffer_size);
extern int rtl8139_get_mac(uint8_t* mac_out);

extern int virtio_net_send_packet(const void* data, size_t length);
extern int virtio_net_receive_packet(void* buffer, size_t buffer_size);
extern int virtio_net_get_mac(uint8_t* mac_out);

extern int rtl8111_init(pci_device_t* pci_dev);
extern int rtl8111_send_packet(const void* data, size_t length);
extern int rtl8111_receive_packet(void* buffer, size_t buffer_size);
extern int rtl8111_get_mac(uint8_t* mac_out);

static nic_driver_t active_nic_driver = {0};
static int nic_initialized = 0;

static int register_e1000(pci_device_t* dev) {
    if (e1000_init(dev) == 0) {
        active_nic_driver.name = "e1000";
        active_nic_driver.init = e1000_init;
        active_nic_driver.send_packet = e1000_send_packet;
        active_nic_driver.receive_packet = e1000_receive_packet;
        active_nic_driver.get_mac_address = e1000_get_mac;
        return 0;
    }
    return -1;
}

static int register_rtl8139(pci_device_t* dev) {
    if (rtl8139_init(dev) == 0) {
        active_nic_driver.name = "rtl8139";
        active_nic_driver.init = rtl8139_init;
        active_nic_driver.send_packet = rtl8139_send_packet;
        active_nic_driver.receive_packet = rtl8139_receive_packet;
        active_nic_driver.get_mac_address = rtl8139_get_mac;
        return 0;
    }
    return -1;
}

static int register_virtio_net(pci_device_t* dev) {
    if (virtio_net_init(dev) == 0) {
        active_nic_driver.name = "virtio-net";
        active_nic_driver.init = virtio_net_init;
        active_nic_driver.send_packet = virtio_net_send_packet;
        active_nic_driver.receive_packet = virtio_net_receive_packet;
        active_nic_driver.get_mac_address = virtio_net_get_mac;
        return 0;
    }
    return -1;
}

static int register_rtl8111(pci_device_t* dev) {
    if (rtl8111_init(dev) == 0) {
        active_nic_driver.name = "rtl8111";
        active_nic_driver.init = rtl8111_init;
        active_nic_driver.send_packet = rtl8111_send_packet;
        active_nic_driver.receive_packet = rtl8111_receive_packet;
        active_nic_driver.get_mac_address = rtl8111_get_mac;
        return 0;
    }
    return -1;
}

int nic_init(void) {
    if (nic_initialized) return 0;

    pci_device_t pci_dev;

    if (pci_find_device(0x10EC, 0x8168, &pci_dev)) {
        if (register_rtl8111(&pci_dev) == 0) {
            nic_initialized = 1;
            return 0;
        }
    }

    if (pci_find_device(0x10EC, 0x8139, &pci_dev)) {
        if (register_rtl8139(&pci_dev) == 0) {
            nic_initialized = 1;
            return 0;
        }
    }


    if (pci_find_device(0x1AF4, 0x1000, &pci_dev)) {
        if (register_virtio_net(&pci_dev) == 0) {
            nic_initialized = 1;
            return 0;
        }
    }
    
    if (pci_find_device(0x1AF4, 0x1041, &pci_dev)) {
        if (register_virtio_net(&pci_dev) == 0) {
            nic_initialized = 1;
            return 0;
        }
    }

    if (pci_find_device(0x8086, 0x100E, &pci_dev)) {
        if (register_e1000(&pci_dev) == 0) {
            nic_initialized = 1;
            return 0;
        }
    }

    return -1; 
}

nic_driver_t* nic_get_driver(void) {
    if (!nic_initialized) return NULL;
    return &active_nic_driver;
}

int nic_send_packet(const void* data, size_t length) {
    if (!nic_initialized || !active_nic_driver.send_packet) return -1;
    return active_nic_driver.send_packet(data, length);
}

int nic_receive_packet(void* buffer, size_t buffer_size) {
    if (!nic_initialized || !active_nic_driver.receive_packet) return 0;
    return active_nic_driver.receive_packet(buffer, buffer_size);
}

int nic_get_mac_address(uint8_t* mac_out) {
    if (!nic_initialized || !active_nic_driver.get_mac_address) return -1;
    return active_nic_driver.get_mac_address(mac_out);
}

const char* nic_get_active_name(void) {
    if (!nic_initialized || !active_nic_driver.name) return NULL;
    return active_nic_driver.name;
}
