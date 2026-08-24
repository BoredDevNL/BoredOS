// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pagecache.h"
#include "slab.h"
#include "mmu.h"
#include "platform.h"
#include "kutils.h"
#include <string.h>

#define PAGE_WAIT_TABLE_SIZE 64
static wait_queue_head_t page_wait_table[PAGE_WAIT_TABLE_SIZE];

static spinlock_t lru_lock = SPINLOCK_INIT;
static page_t *lru_active_head = NULL;
static page_t *lru_inactive_head = NULL;
static size_t nr_active_pages = 0;
static size_t nr_inactive_pages = 0;

static wait_queue_head_t *get_page_waitqueue(page_t *page) {
    uintptr_t hash = ((uintptr_t)page >> 6) % PAGE_WAIT_TABLE_SIZE;
    return &page_wait_table[hash];
}

uint64_t g_dirty_pages_count = 0;
wait_queue_head_t g_dirty_throttle_waitq;

extern void flusher_wake(void);

void pagecache_init(void) {
    for (int i = 0; i < PAGE_WAIT_TABLE_SIZE; i++) {
        wait_queue_init(&page_wait_table[i]);
    }
    wait_queue_init(&g_dirty_throttle_waitq);
    lru_active_head = NULL;
    lru_inactive_head = NULL;
    nr_active_pages = 0;
    nr_inactive_pages = 0;
    g_dirty_pages_count = 0;
}

void address_space_init(address_space_t *mapping, void *host_inode, const address_space_ops_t *a_ops) {
    if (!mapping) return;
    mapping->host_inode = host_inode;
    vmm_rwsem_init(&mapping->i_rwsem);
    mapping->tree_lock = SPINLOCK_INIT;
    radix_tree_init(&mapping->page_tree);
    mapping->i_mmap = NULL;
    mapping->nr_pages = 0;
    mapping->wb_err = 0;
    mapping->a_ops = a_ops;
}

static void lru_add_inactive(page_t *page) {
    uint64_t flags = spinlock_acquire_irqsave(&lru_lock);
    page->lru_prev = NULL;
    page->lru_next = lru_inactive_head;
    if (lru_inactive_head) lru_inactive_head->lru_prev = page;
    lru_inactive_head = page;
    nr_inactive_pages++;
    spinlock_release_irqrestore(&lru_lock, flags);
}

static void lru_remove(page_t *page) {
    uint64_t flags = spinlock_acquire_irqsave(&lru_lock);
    if (page->lru_prev) page->lru_prev->lru_next = page->lru_next;
    else if (lru_inactive_head == page) lru_inactive_head = page->lru_next;
    else if (lru_active_head == page) lru_active_head = page->lru_next;

    if (page->lru_next) page->lru_next->lru_prev = page->lru_prev;
    page->lru_prev = NULL;
    page->lru_next = NULL;
    spinlock_release_irqrestore(&lru_lock, flags);
}

void page_lock(page_t *page) {
    if (!page) return;
    wait_queue_head_t *wq = get_page_waitqueue(page);
    while (true) {
        uint32_t old = __atomic_fetch_or(&page->flags, PAGE_FLAG_LOCKED, __ATOMIC_ACQUIRE);
        if (!(old & PAGE_FLAG_LOCKED)) return;
        wait_queue_wait(wq);
    }
}

bool page_trylock(page_t *page) {
    if (!page) return false;
    uint32_t old = __atomic_fetch_or(&page->flags, PAGE_FLAG_LOCKED, __ATOMIC_ACQUIRE);
    return !(old & PAGE_FLAG_LOCKED);
}

void page_unlock(page_t *page) {
    if (!page) return;
    __atomic_fetch_and(&page->flags, ~PAGE_FLAG_LOCKED, __ATOMIC_RELEASE);
    wait_queue_head_t *wq = get_page_waitqueue(page);
    wait_queue_wake_all(wq);
}

void page_wait_locked(page_t *page) {
    if (!page) return;
    wait_queue_head_t *wq = get_page_waitqueue(page);
    while (page->flags & PAGE_FLAG_LOCKED) {
        wait_queue_wait(wq);
    }
}

page_t *pagecache_find_page(address_space_t *mapping, uint64_t index) {
    if (!mapping) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
    page_t *p = (page_t *)radix_tree_lookup(&mapping->page_tree, index);
    if (p) {
        __atomic_add_fetch(&p->refcount, 1, __ATOMIC_SEQ_CST);
    }
    spinlock_release_irqrestore(&mapping->tree_lock, flags);
    return p;
}

int pagecache_add_page(address_space_t *mapping, page_t *page, uint64_t index) {
    if (!mapping || !page) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
    int ret = radix_tree_insert(&mapping->page_tree, index, page);
    if (ret == 0) {
        page->mapping = mapping;
        page->index = index;
        __atomic_or_fetch(&page->flags, PAGE_FLAG_CACHED, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&page->refcount, 1, __ATOMIC_SEQ_CST);
        mapping->nr_pages++;
    }
    spinlock_release_irqrestore(&mapping->tree_lock, flags);

    if (ret == 0) {
        lru_add_inactive(page);
    }
    return ret;
}

void pagecache_remove_page(address_space_t *mapping, page_t *page) {
    if (!mapping || !page) return;

    if (page->flags & PAGE_FLAG_DIRTY) {
        pagecache_clear_dirty(page);
    }

    lru_remove(page);

    uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
    radix_tree_delete(&mapping->page_tree, page->index);
    page->mapping = NULL;
    __atomic_and_fetch(&page->flags, ~PAGE_FLAG_CACHED, __ATOMIC_SEQ_CST);
    if (mapping->nr_pages > 0) mapping->nr_pages--;
    spinlock_release_irqrestore(&mapping->tree_lock, flags);

    pmm_free_page(page);
}

page_t *pagecache_get_page(address_space_t *mapping, uint64_t index) {
    if (!mapping) return NULL;

    page_t *page = pagecache_find_page(mapping, index);
    if (page) {
        page_wait_locked(page);
        return page;
    }

    page_t *new_page = pmm_alloc_page(PAGE_FLAG_ZERO);
    if (!new_page) return NULL;

    page_lock(new_page);

    int ret = pagecache_add_page(mapping, new_page, index);
    if (ret != 0) {
        page_unlock(new_page);
        pmm_free_page(new_page);
        page = pagecache_find_page(mapping, index);
        if (page) {
            page_wait_locked(page);
            return page;
        }
        return NULL;
    }

    if (mapping->a_ops && mapping->a_ops->read_page) {
        int read_err = mapping->a_ops->read_page(mapping, new_page);
        if (read_err != 0) {
            pagecache_remove_page(mapping, new_page);
            page_unlock(new_page);
            return NULL;
        }
    }

    page_unlock(new_page);
    return new_page;
}

void pagecache_mark_dirty(page_t *page) {
    if (!page || !page->mapping) return;
    address_space_t *mapping = page->mapping;

    uint32_t old = __atomic_fetch_or(&page->flags, PAGE_FLAG_DIRTY, __ATOMIC_SEQ_CST);
    if (!(old & PAGE_FLAG_DIRTY)) {
        __atomic_add_fetch(&g_dirty_pages_count, 1, __ATOMIC_SEQ_CST);
        uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
        radix_tree_tag_set(&mapping->page_tree, page->index, RADIX_TREE_TAG_DIRTY);
        spinlock_release_irqrestore(&mapping->tree_lock, flags);
    }
}

void pagecache_clear_dirty(page_t *page) {
    if (!page || !page->mapping) return;
    address_space_t *mapping = page->mapping;

    uint32_t old = __atomic_fetch_and(&page->flags, ~PAGE_FLAG_DIRTY, __ATOMIC_SEQ_CST);
    if (old & PAGE_FLAG_DIRTY) {
        if (__atomic_sub_fetch(&g_dirty_pages_count, 1, __ATOMIC_SEQ_CST) == 0) {
            wait_queue_wake_all(&g_dirty_throttle_waitq);
        } else {
            pmm_stats_t stats = pmm_get_stats();
            uint64_t limit = (stats.total_pages * 20) / 100;
            if (g_dirty_pages_count < limit) {
                wait_queue_wake_all(&g_dirty_throttle_waitq);
            }
        }
        uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
        radix_tree_tag_clear(&mapping->page_tree, page->index, RADIX_TREE_TAG_DIRTY);
        spinlock_release_irqrestore(&mapping->tree_lock, flags);
    }
}

void balance_dirty_pages(void) {
    pmm_stats_t stats = pmm_get_stats();
    if (stats.total_pages == 0) return;

    uint64_t wake_watermark = (stats.total_pages * 10) / 100;
    uint64_t hard_limit = (stats.total_pages * 20) / 100;

    if (g_dirty_pages_count >= wake_watermark) {
        flusher_wake();
    }

    while (g_dirty_pages_count >= hard_limit) {
        flusher_wake();
        wait_queue_wait_timeout(&g_dirty_throttle_waitq, 10);
    }
}

int pagecache_sync_mapping(address_space_t *mapping) {
    if (!mapping || !mapping->a_ops || !mapping->a_ops->write_page) return 0;

    void *dirty_pages[16];
    uint64_t next_index = 0;
    int synced_count = 0;

    while (true) {
        uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
        size_t found = radix_tree_gang_lookup_tag(&mapping->page_tree, dirty_pages, next_index, 16, RADIX_TREE_TAG_DIRTY);
        if (found == 0) {
            spinlock_release_irqrestore(&mapping->tree_lock, flags);
            break;
        }

        for (size_t i = 0; i < found; i++) {
            page_t *p = (page_t *)dirty_pages[i];
            __atomic_add_fetch(&p->refcount, 1, __ATOMIC_SEQ_CST);
        }
        page_t *last_page = (page_t *)dirty_pages[found - 1];
        next_index = last_page->index + 1;
        spinlock_release_irqrestore(&mapping->tree_lock, flags);

        for (size_t i = 0; i < found; i++) {
            page_t *p = (page_t *)dirty_pages[i];
            page_lock(p);
            if (p->flags & PAGE_FLAG_DIRTY) {
                pagecache_clear_dirty(p);
                int err = mapping->a_ops->write_page(mapping, p);
                if (err != 0) {
                    pagecache_mark_dirty(p);
                    errseq_set(&mapping->wb_err, 5); // EIO
                    __atomic_or_fetch(&p->flags, PAGE_FLAG_ERROR, __ATOMIC_SEQ_CST);
                } else {
                    synced_count++;
                }
            }
            page_unlock(p);
            pmm_free_page(p);
        }
    }
    return synced_count;
}


void address_space_add_vma(address_space_t *mapping, vm_area_t *vma) {
    if (!mapping || !vma) return;
    vmm_down_write(&mapping->i_rwsem);
    vma->i_mmap_next = mapping->i_mmap;
    mapping->i_mmap = vma;
    vmm_up_write(&mapping->i_rwsem);
}

void address_space_remove_vma(address_space_t *mapping, vm_area_t *vma) {
    if (!mapping || !vma) return;
    vmm_down_write(&mapping->i_rwsem);
    vm_area_t **curr = &mapping->i_mmap;
    while (*curr) {
        if (*curr == vma) {
            *curr = vma->i_mmap_next;
            vma->i_mmap_next = NULL;
            break;
        }
        curr = &(*curr)->i_mmap_next;
    }
    vmm_up_write(&mapping->i_rwsem);
}

static void unmap_mapping_page_vmas(address_space_t *mapping, uint64_t index) {
    vm_area_t *vma = mapping->i_mmap;
    while (vma) {
        uint64_t file_page_start = vma->file_offset >> 12;
        uint64_t vma_page_count = (vma->end - vma->start) >> 12;
        if (index >= file_page_start && index < file_page_start + vma_page_count) {
            uintptr_t vaddr = vma->start + ((index - file_page_start) << 12);
            if (vma->space && vma->space->mmu_ctx) {
                mmu_unmap_page(vma->space->mmu_ctx, vaddr);
            }
        }
        vma = vma->i_mmap_next;
    }
}

void pagecache_truncate_range(address_space_t *mapping, uint64_t start_byte) {
    if (!mapping) return;

    vmm_down_write(&mapping->i_rwsem);

    uint64_t start_index = (start_byte + 4095) >> 12;
    uint32_t slack_offset = start_byte & 4095;

    if (slack_offset != 0) {
        uint64_t partial_index = start_byte >> 12;
        page_t *partial = pagecache_find_page(mapping, partial_index);
        if (partial) {
            page_lock(partial);
            uintptr_t phys = pmm_page_to_paddr(partial);
            memset((void *)p2v(phys + slack_offset), 0, 4096 - slack_offset);
            pagecache_mark_dirty(partial);
            page_unlock(partial);
            pmm_free_page(partial);
        }
    }

    void *pages[16];
    uint64_t next_idx = start_index;

    while (true) {
        uint64_t flags = spinlock_acquire_irqsave(&mapping->tree_lock);
        size_t found = radix_tree_gang_lookup(&mapping->page_tree, pages, next_idx, 16);
        if (found == 0) {
            spinlock_release_irqrestore(&mapping->tree_lock, flags);
            break;
        }

        for (size_t i = 0; i < found; i++) {
            page_t *p = (page_t *)pages[i];
            __atomic_add_fetch(&p->refcount, 1, __ATOMIC_SEQ_CST);
        }
        page_t *last_page = (page_t *)pages[found - 1];
        next_idx = last_page->index + 1;
        spinlock_release_irqrestore(&mapping->tree_lock, flags);

        for (size_t i = 0; i < found; i++) {
            page_t *p = (page_t *)pages[i];
            page_lock(p);
            unmap_mapping_page_vmas(mapping, p->index);
            pagecache_remove_page(mapping, p);
            page_unlock(p);
            pmm_free_page(p);
        }
    }

    vmm_up_write(&mapping->i_rwsem);
}

void address_space_destroy(address_space_t *mapping) {
    if (!mapping) return;
    pagecache_truncate_range(mapping, 0);
}

void pagecache_invalidate_all(address_space_t *mapping) {
    if (!mapping) return;
    pagecache_sync_mapping(mapping);
    pagecache_truncate_range(mapping, 0);
}

#include "disk.h"
#include "vfs.h"

int pagecache_sync_all(void) {
    return vfs_sync_all();
}

int sync_blockdev(Disk *disk) {
    if (!disk) return -1;
    pagecache_sync_all();
    if (disk->sync) {
        return disk->sync(disk);
    }
    return 0;
}


