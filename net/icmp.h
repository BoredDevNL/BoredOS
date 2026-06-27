// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "network.h"

void icmp_handle_packet(ipv4_address_t src, void *data, uint16_t len);
int cli_cmd_ping_syscall(ipv4_address_t *dest);

#endif
