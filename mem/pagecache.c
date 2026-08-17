// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pagecache.h"
#include <stddef.h>

void pagecache_init(void) {
}

page_t *pagecache_get_page(void *inode, uint64_t offset) {
    (void)inode; (void)offset;
    return NULL;
}

int pagecache_put_page(void *inode, uint64_t offset, page_t *page) {
    (void)inode; (void)offset; (void)page;
    return 0;
}

void pagecache_mark_dirty(page_t *page) {
    if (page) page->flags |= PAGE_FLAG_DIRTY;
}

int pagecache_sync_inode(void *inode) {
    (void)inode;
    return 0;
}

int pagecache_sync_all(void) {
    return 0;
}

void pagecache_start_flusher_daemon(void) {
}
