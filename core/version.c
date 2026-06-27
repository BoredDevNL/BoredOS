// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "version.h"
#include <stddef.h>


void get_os_info(os_info_t *info) {
    if (!info) return;

    char *p = (char *)info;
    for (size_t i = 0; i < sizeof(os_info_t); i++) p[i] = 0;

    const char *os_name = "BoredOS";
    const char *os_version = "26.7.0-dev";
    const char *os_codename = "Aimless Albatross";
    const char *kernel_name = "Boredkernel";
    const char *kernel_version = "5.0.0-dev";
    const char *build_date = __DATE__;
    const char *build_time = __TIME__;
    const char *build_arch = "x86_64";

    int j;
    j = 0; while (os_name[j] && j < 63) { info->os_name[j] = os_name[j]; j++; } info->os_name[j] = '\0';
    j = 0; while (os_version[j] && j < 63) { info->os_version[j] = os_version[j]; j++; } info->os_version[j] = '\0';
    j = 0; while (os_codename[j] && j < 63) { info->os_codename[j] = os_codename[j]; j++; } info->os_codename[j] = '\0';
    j = 0; while (kernel_name[j] && j < 63) { info->kernel_name[j] = kernel_name[j]; j++; } info->kernel_name[j] = '\0';
    j = 0; while (kernel_version[j] && j < 63) { info->kernel_version[j] = kernel_version[j]; j++; } info->kernel_version[j] = '\0';
    j = 0; while (build_date[j] && j < 63) { info->build_date[j] = build_date[j]; j++; } info->build_date[j] = '\0';
    j = 0; while (build_time[j] && j < 63) { info->build_time[j] = build_time[j]; j++; } info->build_time[j] = '\0';
    j = 0; while (build_arch[j] && j < 63) { info->build_arch[j] = build_arch[j]; j++; } info->build_arch[j] = '\0';
}
