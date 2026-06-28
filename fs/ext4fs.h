// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Abdelkader Boudih
#ifndef EXT4FS_H
#define EXT4FS_H

#include "vfs.h"
#include <ext4_types.h>

void* ext4fs_mount_volume(void *disk_ptr);
void ext4fs_umount_volume(void *fs_private);
vfs_fs_ops_t* ext4fs_get_ops(void);

#endif
