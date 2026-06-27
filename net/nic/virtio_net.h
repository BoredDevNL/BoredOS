// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stddef.h>
#include "pci.h"

int virtio_net_init(pci_device_t* pci_dev);
int virtio_net_send_packet(const void* data, size_t length);
int virtio_net_receive_packet(void* buffer, size_t buffer_size);
int virtio_net_get_mac(uint8_t* mac_out);

#endif
