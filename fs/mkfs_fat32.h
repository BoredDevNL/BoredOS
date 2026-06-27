// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef MKFS_FAT32_H
#define MKFS_FAT32_H

#include <stdint.h>
#include "disk.h"
#define MIN_FAT32_SECTORS 65536
int mkfs_fat32_format(Disk *disk, uint32_t sector_count, const char *label);

#endif
