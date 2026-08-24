# Page Cache & Writeback

Source: [`mem/pagecache.c`](../../mem/pagecache.c), [`mem/pagecache.h`](../../mem/pagecache.h), [`mem/radix_tree.c`](../../mem/radix_tree.c), [`mem/radix_tree.h`](../../mem/radix_tree.h), [`fs/flusher.c`](../../fs/flusher.c), [`fs/flusher.h`](../../fs/flusher.h)

The page cache caches disk blocks and file data in physical RAM pages. It is used by filesystems (FAT32, ext4), RAM-backed filesystems ([tmpfs](../storage/filesystem.md)), and block devices.

## Data Structures

### Address Space (`address_space_t`)

Each cached file or block device holds an `address_space_t`:
- `page_tree`: 64-way radix tree indexing `page_t` pointers by page offset (`file_offset >> 12`).
- `tree_lock`: Spinlock protecting radix tree lookups and mutations.
- `i_rwsem`: Reader-writer semaphore for serialized file I/O.
- `a_ops`: Pointer to `address_space_ops_t` containing `read_page`, `write_page`, and `bmap` hooks.
- `nr_pages`: Total number of cached pages.
- `wb_err`: Writeback error sequence counter.

### Radix Tree

Source: [`mem/radix_tree.c`](../../mem/radix_tree.c), [`mem/radix_tree.h`](../../mem/radix_tree.h)

Cached pages are indexed in a 64-way branching tree (`radix_tree_root_t`). Each level covers 6 bits of page index.
Nodes maintain bitmasks for slot tags:
- `RADIX_TREE_TAG_DIRTY`: Marks dirty pages pending writeback.
- `RADIX_TREE_TAG_LOCKED`: Marks pages currently locked for I/O.

## Page Cache API

- `address_space_init(mapping, host_inode, a_ops)`: Initializes mapping.
- `address_space_destroy(mapping)`: Invalidates pages and cleans up tree.
- `pagecache_get_page(mapping, index)`: Retrieves a page at `index`, allocating a new one from PMM if not present.
- `pagecache_find_page(mapping, index)`: Look up an existing page without allocating.
- `pagecache_add_page(mapping, page, index)`: Inserts a page into the tree.
- `pagecache_remove_page(mapping, page)`: Removes a page from the tree.
- `pagecache_mark_dirty(page)` / `pagecache_clear_dirty(page)`: Updates dirty tags and system-wide dirty counter `g_dirty_pages_count`.
- `pagecache_readahead(mapping, start_index, count)`: Populates adjacent pages from storage.
- `pagecache_truncate_range(mapping, new_size)`: Truncates cached pages past `new_size`.
- `pagecache_invalidate_all(mapping)`: Clears all cached pages.
- `pagecache_sync_mapping(mapping)`: Writes dirty pages in mapping to disk.
- `pagecache_sync_all()`: Flushes all dirty pages in the system.
- `sync_blockdev(disk)`: Syncs cached blocks for a block device.

## Page Locking

- `page_lock(page)`: Acquires page lock via wait queue.
- `page_unlock(page)`: Releases page lock and wakes waiters.
- `page_trylock(page)`: Non-blocking page lock attempt.
- `page_wait_locked(page)`: Waits until page lock is released.

## Writeback and Dirty Throttling

- `balance_dirty_pages()`: Called during write paths. If `g_dirty_pages_count` exceeds 20% of total memory, writers sleep on `g_dirty_throttle_waitq`.
- Flusher thread (`fs/flusher.c`): A background kernel thread running `flusher_worker_loop()` wakes every 3 seconds or on `flusher_wake()`. It calls `vfs_sync_all()` and unblocks writers once dirty pages drop below the limit.
