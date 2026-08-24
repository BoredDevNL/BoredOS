// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "flusher.h"
#include "pagecache.h"
#include "wait_queue.h"
#include "process.h"
#include "vfs.h"
#include "pmm.h"

static wait_queue_head_t flusher_waitq;

void flusher_wake(void) {
    wait_queue_wake_all(&flusher_waitq);
}

static void flusher_worker_loop(void) {
    while (1) {
        wait_queue_wait_timeout(&flusher_waitq, 3000);

        if (g_dirty_pages_count > 0) {
            vfs_sync_all();

            pmm_stats_t stats = pmm_get_stats();
            uint64_t limit = (stats.total_pages * 20) / 100;
            if (g_dirty_pages_count < limit) {
                wait_queue_wake_all(&g_dirty_throttle_waitq);
            }
        }
    }
}

void flusher_init(void) {
    wait_queue_init(&flusher_waitq);
    process_create(flusher_worker_loop, false);
}
