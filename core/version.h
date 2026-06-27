// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
#ifndef VERSION_H
#define VERSION_H

typedef struct {
    char os_name[64];
    char os_version[64];
    char os_codename[64];
    char kernel_name[64];
    char kernel_version[64];
    char build_date[64];
    char build_time[64];
    char build_arch[64];
} os_info_t;

void get_os_info(os_info_t *info);

#endif // VERSION_H
