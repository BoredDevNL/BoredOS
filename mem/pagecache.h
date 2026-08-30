// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef BOREDOS_PAGECACHE_H
#define BOREDOS_PAGECACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "pmm.h"
#include "radix_tree.h"
#include "vmm.h"
#include "vma.h"
#include "spinlock.h"
#include "wait_queue.h"

#define PAGE_FLAG_UPTODATE (1 << 14)
#define PAGE_FLAG_ERROR    (1 << 15)

typedef uint32_t errseq_t;

#define ERRSEQ_SEEN     (1u << 0)
#define ERRSEQ_ERR_MASK 0x7FFE
#define ERRSEQ_CTR_INC  0x8000

static inline errseq_t errseq_set(errseq_t *eseq, int err) {
    errseq_t cur = *eseq;
    errseq_t new_val = (cur + ERRSEQ_CTR_INC) & ~ERRSEQ_SEEN;
    new_val |= ((uint32_t)(err & 0xFFF) << 1);
    *eseq = new_val;
    return new_val;
}

static inline errseq_t errseq_sample(errseq_t *eseq) {
    return *eseq;
}

static inline int errseq_check_and_advance(errseq_t *eseq, errseq_t *since) {
    errseq_t cur = *eseq;
    if (cur != *since) {
        *since = cur | ERRSEQ_SEEN;
        return (int)((cur & ERRSEQ_ERR_MASK) >> 1);
    }
    return 0;
}

struct address_space;

typedef struct vma_interval_node {
    vm_area_t *vma;
    struct vma_interval_node *next;
} vma_interval_node_t;

typedef struct address_space_ops {
    int (*read_page)(struct address_space *mapping, page_t *page);
    int (*write_page)(struct address_space *mapping, page_t *page);
    int (*bmap)(struct address_space *mapping, uint64_t block_index, uint64_t *out_phys_block);
} address_space_ops_t;

typedef struct address_space {
    void *host_inode;
    vmm_rwsem_t i_rwsem;
    spinlock_t tree_lock;
    radix_tree_root_t page_tree;
    struct vm_area *i_mmap;
    uint64_t nr_pages;
    errseq_t wb_err;
    const address_space_ops_t *a_ops;
} address_space_t;

extern uint64_t g_dirty_pages_count;
extern wait_queue_head_t g_dirty_throttle_waitq;

void pagecache_init(void);

void address_space_init(address_space_t *mapping, void *host_inode, const address_space_ops_t *a_ops);
void address_space_destroy(address_space_t *mapping);

void page_lock(page_t *page);
void page_unlock(page_t *page);
bool page_trylock(page_t *page);
void page_wait_locked(page_t *page);

page_t *pagecache_get_page(address_space_t *mapping, uint64_t index);
page_t *pagecache_find_page(address_space_t *mapping, uint64_t index);
int pagecache_add_page(address_space_t *mapping, page_t *page, uint64_t index);
void pagecache_remove_page(address_space_t *mapping, page_t *page);
void pagecache_mark_dirty(page_t *page);
void pagecache_clear_dirty(page_t *page);

void pagecache_readahead(address_space_t *mapping, uint64_t start_index, size_t count);
void pagecache_truncate_range(address_space_t *mapping, uint64_t new_size);
void pagecache_invalidate_all(address_space_t *mapping);

int pagecache_sync_mapping(address_space_t *mapping);
int pagecache_sync_all(void);
void balance_dirty_pages(void);

struct Disk;
int sync_blockdev(struct Disk *disk);

void address_space_add_vma(address_space_t *mapping, vm_area_t *vma);
void address_space_remove_vma(address_space_t *mapping, vm_area_t *vma);

size_t pagecache_reclaim(size_t nr_to_reclaim);

#endif // BOREDOS_PAGECACHE_H
