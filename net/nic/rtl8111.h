// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef RTL8111_H
#define RTL8111_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

int rtl8111_init(pci_device_t* pci_dev);
int rtl8111_send_packet(const void* data, size_t length);
int rtl8111_receive_packet(void* buffer, size_t buffer_size);
int rtl8111_get_mac(uint8_t* mac_out);

#endif
