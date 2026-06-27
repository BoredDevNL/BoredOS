// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef BOOTFS_H
#define BOOTFS_H

#include "vfs.h"

void bootfs_init(void);
void bootfs_mount_boot_partition(void);
void bootfs_refresh_from_disk(void);
vfs_fs_ops_t* bootfs_get_ops(void);
void bootfs_register_file(const char *name, void *data, uint32_t size);

#endif
