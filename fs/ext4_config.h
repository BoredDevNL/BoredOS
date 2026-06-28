// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Abdelkader Boudih

#ifndef EXT4_CONFIG_BOREDOS_H
#define EXT4_CONFIG_BOREDOS_H

#define CONFIG_USE_DEFAULT_CFG 1

#define CONFIG_HAVE_OWN_ERRNO 1

#define CONFIG_DEBUG_PRINTF 0
#define CONFIG_DEBUG_ASSERT 0

#define CONFIG_BLOCK_DEV_CACHE_SIZE 16
#define CONFIG_BLOCK_DEV_ENABLE_STATS 0

#define CONFIG_EXT4_BLOCKDEVS_COUNT 16
#define CONFIG_EXT4_MOUNTPOINTS_COUNT 16

#define CONFIG_UNALIGNED_ACCESS 1

#define CONFIG_USE_USER_MALLOC 1

#include <stddef.h>
void *ext4_user_malloc(size_t size);
void *ext4_user_calloc(size_t n, size_t size);
void  ext4_user_free(void *ptr);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif
