// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef NIC_H
#define NIC_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

typedef struct nic_driver {
    const char* name;
    int (*init)(pci_device_t* pci_dev);
    int (*send_packet)(const void* data, size_t length);
    int (*receive_packet)(void* buffer, size_t buffer_size);
    int (*get_mac_address)(uint8_t* mac_out);
} nic_driver_t;

int nic_init(void);
nic_driver_t* nic_get_driver(void);
int nic_send_packet(const void* data, size_t length);
int nic_receive_packet(void* buffer, size_t buffer_size);
int nic_get_mac_address(uint8_t* mac_out);

#endif
