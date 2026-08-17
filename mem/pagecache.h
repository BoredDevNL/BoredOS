// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
/*
################################################################

BEWARE

THIS IS SCAFFOLDING, CAN VERY MUCH BE CHANGED!!

#################################################################

*/

#ifndef BOREDOS_PAGECACHE_H
#define BOREDOS_PAGECACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pmm.h"

typedef struct page_cache_entry {
    void *inode;
    uint64_t offset;
    page_t *page;
    bool is_dirty;
    struct page_cache_entry *next;
} page_cache_entry_t;

void pagecache_init(void);

page_t *pagecache_get_page(void *inode, uint64_t offset);
int pagecache_put_page(void *inode, uint64_t offset, page_t *page);
void pagecache_mark_dirty(page_t *page);

int pagecache_sync_inode(void *inode);
int pagecache_sync_all(void);

void pagecache_start_flusher_daemon(void);

#endif // BOREDOS_PAGECACHE_H
