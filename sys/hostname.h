// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef KERNEL_HOSTNAME_H
#define KERNEL_HOSTNAME_H

#include <stddef.h>

#define MAX_HOSTNAME_LEN 64

#ifdef __cplusplus
extern "C" {
#endif

void kernel_get_hostname(char *buf, size_t max_len);
int kernel_set_hostname(const char *name, size_t len);
void hostname_init(void);

#ifdef __cplusplus
}
#endif

#endif // KERNEL_HOSTNAME_H
