// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef TAR_H
#define TAR_H

#include <stdint.h>
#include <stddef.h>

// Parse a TAR archive located in memory and extract its contents into the current filesystem (fatal32 RAM disk).
void tar_parse(void *archive, uint64_t archive_size);

#endif
