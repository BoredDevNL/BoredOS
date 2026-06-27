#include "shm.h"
#include "vfs.h"
#include "spinlock.h"
#include "memory_manager.h"
#include "kutils.h"
#include "platform.h"

extern void serial_write(const char *str);

static shm_segment_t *shm_list = NULL;
static spinlock_t shm_lock = SPINLOCK_INIT;

shm_segment_t* shm_get_or_create(const char *name) {
    if (!name || name[0] == '\0') return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    shm_segment_t *cur = shm_list;
    while (cur) {
        // Safe string comparison
        int match = 1;
        for (int i = 0; name[i] || cur->name[i]; i++) {
            if (name[i] != cur->name[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            cur->ref_count++;
            spinlock_release_irqrestore(&shm_lock, flags);
            return cur;
        }
        cur = cur->next;
    }

    // Not found, create new segment
    shm_segment_t *seg = (shm_segment_t *)kmalloc(sizeof(shm_segment_t));
    if (!seg) {
        spinlock_release_irqrestore(&shm_lock, flags);
        return NULL;
    }

    // Initialize string safely
    int i = 0;
    for (; name[i] && i < 63; i++) {
        seg->name[i] = name[i];
    }
    seg->name[i] = '\0';
    seg->page_count = 0;
    seg->size = 0;
    seg->ref_count = 1;
    seg->next = shm_list;
    shm_list = seg;

    spinlock_release_irqrestore(&shm_lock, flags);
    return seg;
}

void shm_ref(shm_segment_t *seg) {
    if (!seg) return;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    seg->ref_count++;
    spinlock_release_irqrestore(&shm_lock, flags);
}

void shm_unref(shm_segment_t *seg) {
    if (!seg) return;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    seg->ref_count--;
    if (seg->ref_count <= 0) {
        // Remove from list
        shm_segment_t *prev = NULL;
        shm_segment_t *cur = shm_list;
        while (cur) {
            if (cur == seg) {
                if (prev) {
                    prev->next = cur->next;
                } else {
                    shm_list = cur->next;
                }
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        
        // Free backing physical pages
        for (uint32_t i = 0; i < seg->page_count; i++) {
            kfree((void *)p2v(seg->phys_pages[i]));
        }
        kfree(seg);
    }
    spinlock_release_irqrestore(&shm_lock, flags);
}

int shm_allocate(shm_segment_t *seg, size_t size) {
    if (!seg) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    
    size_t aligned_len = (size + 4095) & ~4095ULL;
    uint32_t pages_needed = aligned_len / 4096;
    if (pages_needed > SHM_MAX_PAGES) {
        spinlock_release_irqrestore(&shm_lock, flags);
        return -1;
    }

    if (pages_needed > seg->page_count) {
        for (uint32_t i = seg->page_count; i < pages_needed; i++) {
            void *page = kmalloc_aligned(4096, 4096);
            if (!page) {
                // Rollback newly allocated pages
                for (uint32_t j = seg->page_count; j < i; j++) {
                    kfree((void *)p2v(seg->phys_pages[j]));
                }
                spinlock_release_irqrestore(&shm_lock, flags);
                return -1;
            }
            
            // Zero out the page
            memset(page, 0, 4096);

            seg->phys_pages[i] = v2p((uint64_t)page);
        }
        seg->page_count = pages_needed;
    }

    if (aligned_len > seg->size) {
        seg->size = (uint32_t)aligned_len;
    }
    
    spinlock_release_irqrestore(&shm_lock, flags);
    return 0;
}

void shm_unlink(const char *name) {
    if (!name || name[0] == '\0') return;

    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    shm_segment_t *cur = shm_list;
    while (cur) {
        int match = 1;
        for (int i = 0; name[i] || cur->name[i]; i++) {
            if (name[i] != cur->name[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            // Remove from global list
            shm_segment_t *prev = NULL;
            shm_segment_t *temp = shm_list;
            while (temp) {
                if (temp == cur) {
                    if (prev) {
                        prev->next = temp->next;
                    } else {
                        shm_list = temp->next;
                    }
                    break;
                }
                prev = temp;
                temp = temp->next;
            }

            // Decrement the reference count representing the namespace entry
            cur->ref_count--;
            if (cur->ref_count <= 0) {
                for (uint32_t i = 0; i < cur->page_count; i++) {
                    kfree((void *)p2v(cur->phys_pages[i]));
                }
                kfree(cur);
            }
            break;
        }
        cur = cur->next;
    }
    spinlock_release_irqrestore(&shm_lock, flags);
}

bool shm_exists(const char *name) {
    if (!name || name[0] == '\0') return false;

    uint64_t flags = spinlock_acquire_irqsave(&shm_lock);
    shm_segment_t *cur = shm_list;
    while (cur) {
        int match = 1;
        for (int i = 0; name[i] || cur->name[i]; i++) {
            if (name[i] != cur->name[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            spinlock_release_irqrestore(&shm_lock, flags);
            return true;
        }
        cur = cur->next;
    }
    spinlock_release_irqrestore(&shm_lock, flags);
    return false;
}

