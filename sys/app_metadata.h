// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef APP_METADATA_H
#define APP_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include "elf.h"

bool app_metadata_read(const char *path, boredos_app_metadata_t *out_metadata);
bool app_metadata_get_primary_image(const char *path, char *out_path, size_t out_path_size);

#endif
