// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_TMPFS_H
#define BOREDOS_TMPFS_H

#include "vfs.h"
#include "../mem/pagecache.h"

typedef struct tmpfs_inode {
    char name[256];
    bool is_dir;
    uint32_t size;
    uint32_t attributes;
    address_space_t i_mapping;
    struct tmpfs_inode *parent;
    struct tmpfs_inode *children;
    struct tmpfs_inode *next;
} tmpfs_inode_t;

typedef struct tmpfs_file_handle {
    tmpfs_inode_t *inode;
    uint32_t position;
    uint32_t mode;
} tmpfs_file_handle_t;

void tmpfs_init(void);
void tmpfs_destroy_all(void);
vfs_fs_ops_t *tmpfs_get_ops(void);

#endif // BOREDOS_TMPFS_H
