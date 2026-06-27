// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

// Kernel memory manager — provides kmalloc/kfree/krealloc for the rest of the kernel.
// Uses a slab allocator for small objects (<= 512 B) and a sorted block-list allocator for everything else.

#include "memory_manager.h"
#include <stdint.h>
#include "limine.h"
#include "platform.h"
#include "spinlock.h"
#include "kutils.h"


#define PAGE_SIZE  4096UL
#define SLAB_PAGE_MAGIC     0x534C4142U
#define SLAB_PAGE_MAGIC_INV (~SLAB_PAGE_MAGIC) 

static const uint16_t slab_sizes[SLAB_CLASSES] = {8, 16, 32, 64, 128, 256, 512};

// Each slab page is exactly PAGE_SIZE. Header lives at the start, object slots follow.
// Free slots store a next-pointer at offset 0 (intrusive LIFO free-list).
typedef struct SlabPage {
    uint32_t        magic;
    uint32_t        magic_inv;
    uint16_t        obj_size;
    uint16_t        free_count;
    uint16_t        total_count;
    uint16_t        obj_start;   // byte offset from page base to first slot
    uint16_t        class_idx;
    uint16_t        _pad;
    struct SlabPage *next;
    void            *freelist;
} SlabPage;

typedef struct {
    SlabPage *pages;
    size_t    total_allocs;
    size_t    total_frees;
} SlabCache;

// Block list starts in BSS and migrates to a heap allocation once it fills up.
static MemBlock   _bootstrap_blocks[BLOCK_LIST_INITIAL_CAPACITY];
static MemBlock  *block_list     = _bootstrap_blocks;
static int        block_capacity = BLOCK_LIST_INITIAL_CAPACITY;
static int        block_count    = 0;
static bool       on_heap        = false;
static bool       growing        = false;

static size_t    memory_pool_size   = 0;
static size_t    total_allocated    = 0;
static size_t    peak_allocated     = 0;
static uint32_t  allocation_counter = 0;
static bool      initialized        = false;
static spinlock_t mm_lock           = SPINLOCK_INIT;

static SlabCache slab_caches[SLAB_CLASSES];
static size_t    slab_total_allocs = 0;
static size_t    slab_total_frees  = 0;

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);


static void mem_memmove(void *dest, const void *src, size_t len) {
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) { while (len--) *d++ = *s++; }
    else        { d += len; s += len; while (len--) *(--d) = *(--s); }
}

static void  *_kmalloc_locked(size_t size, size_t alignment);
static void   _kfree_locked(void *ptr);
static bool   insert_block_at(int idx, void *addr, size_t size, bool allocated, uint32_t id);
static void   remove_block_at(int idx);
static bool   grow_block_list(void);

// Returns a sequential allocation ticked counter
static uint32_t get_timestamp(void) {
    static uint32_t tick = 0;
    return tick++;
}

static bool insert_block_at(int idx, void *addr, size_t size, bool allocated, uint32_t id) {
    // Proactive growth: If we're within 10 slots of full, grow the list now.
    // This ensures we always have metadata space to split blocks even during a nested kmalloc.
    if (block_count >= block_capacity - 10 && !growing) {
        grow_block_list();
    }
    if (block_count >= block_capacity) return false;
    for (int j = block_count; j > idx; j--)
        block_list[j] = block_list[j - 1];
    block_list[idx] = (MemBlock){
        .address       = addr,
        .size          = size,
        .allocated     = allocated,
        .allocation_id = id,
        .timestamp     = allocated ? get_timestamp() : 0,
    };
    block_count++;
    return true;
}

static void remove_block_at(int idx) {
    for (int j = idx; j < block_count - 1; j++)
        block_list[j] = block_list[j + 1];
    block_count--;
}

// Splits the chosen block into [head padding | allocation | tail remainder].
// All three parts are tracked as separate MemBlock entries. New memory is zero-filled.
static void *_kmalloc_locked(size_t size, size_t alignment) {
    // Default to 8-byte alignment; this satisfies the strictest scalar type (double/pointer) on x86-64.
    if (alignment == 0) alignment = 8;
    // Round size up to the next 8-byte boundary so every returned block stays naturally aligned.
    size = (size + 7) & ~7ULL;

// First-fit search for a suitable block.
restart:
    for (int i = 0; i < block_count; i++) {
        if (block_list[i].allocated) continue;

        uintptr_t base    = (uintptr_t)block_list[i].address;
        size_t    bsize   = block_list[i].size;
        uintptr_t aligned = (base + alignment - 1) & ~(uintptr_t)(alignment - 1);
        size_t    padding = aligned - base;

        if (bsize < size + padding) continue;

        size_t tail  = bsize - (size + padding);
        int    extra = (padding > 0) + (tail > 0);  // up to 2 new blocks: head padding + tail remainder

        // +2 worst case: padding block + tail block. Grow if needed; restart so indices stay valid.
        if (block_count + extra + 2 > block_capacity) {
            if (grow_block_list()) goto restart;
            if (block_count + extra > block_capacity) continue;
        }

        void    *ptr = (void *)aligned;
        uint32_t id  = ++allocation_counter;

        int cur = i;
        if (padding > 0) {
            block_list[i].size = padding;
            if (!insert_block_at(i + 1, ptr, size, true, id)) continue;
            cur = i + 1;
        } else {
            block_list[i] = (MemBlock){ ptr, size, true, id, get_timestamp() };
        }

        if (tail > 0)
            insert_block_at(cur + 1, (void *)((uintptr_t)ptr + size), tail, false, 0);

        total_allocated += size;
        if (total_allocated > peak_allocated) peak_allocated = total_allocated;
        memset(ptr, 0, size);
        return ptr;
    }
    return NULL;
}

// Frees and coalesces with adjacent free neighbours (right first, then left).
static void _kfree_locked(void *ptr) {
    int i = -1;
    for (int j = 0; j < block_count; j++) {
        if (block_list[j].allocated && block_list[j].address == ptr) { i = j; break; }
    }
    if (i < 0) return;

    total_allocated           -= block_list[i].size;
    block_list[i].allocated    = false;
    block_list[i].allocation_id = 0;

    if (i + 1 < block_count && !block_list[i + 1].allocated &&
        (uintptr_t)block_list[i].address + block_list[i].size ==
        (uintptr_t)block_list[i + 1].address) {
        block_list[i].size += block_list[i + 1].size;
        remove_block_at(i + 1);
    }
    if (i > 0 && !block_list[i - 1].allocated &&
        (uintptr_t)block_list[i - 1].address + block_list[i - 1].size ==
        (uintptr_t)block_list[i].address) {
        block_list[i - 1].size += block_list[i].size;
        remove_block_at(i);
    }
}

// _kmalloc_locked can call grow_block_list again if the block list fills
// during the allocation of the new array, causing infinite recursion without this flag.
static bool grow_block_list(void) {
    if (growing) return false;
    growing = true;

    int new_cap = block_capacity * 2;
    MemBlock *nl = (MemBlock *)_kmalloc_locked((size_t)new_cap * sizeof(MemBlock), 8);
    if (!nl) { growing = false; return false; }

    memcpy(nl, block_list, (size_t)block_count * sizeof(MemBlock));
    
    MemBlock *old_ptr = block_list;
    bool old_on_heap  = on_heap;

    block_list     = nl;
    block_capacity = new_cap;
    on_heap        = true;
    growing        = false;

    if (old_on_heap) _kfree_locked(old_ptr);
    return true;
}

// Uses insertion sort. Only called once at init on a list that is already nearly sorted by address.
static void sort_block_list(void) {
    for (int i = 1; i < block_count; i++) {
        MemBlock key = block_list[i];
        int j = i - 1;
        while (j >= 0 && (uintptr_t)block_list[j].address > (uintptr_t)key.address)
            block_list[j + 1] = block_list[j--];
        block_list[j + 1] = key;
    }
}

// Fragmentation = percentage of free memory stranded outside the largest free block.
static size_t calculate_fragmentation(void) {
    size_t free_total = memory_pool_size - total_allocated;
    if (!free_total || !total_allocated) return 0;
    size_t largest = 0;
    for (int i = 0; i < block_count; i++)
        if (!block_list[i].allocated && block_list[i].size > largest)
            largest = block_list[i].size;
    return 100 - (largest * 100) / free_total;
}

static int slab_class_for_size(size_t size) {
    for (int i = 0; i < SLAB_CLASSES; i++)
        if (size <= slab_sizes[i]) return i;
    return -1;
}

static inline bool slab_ptr_belongs_to_page(const SlabPage *page, const void *ptr) {
    if (!ptr) return false;
    uintptr_t uptr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)page;
    uintptr_t off  = uptr - base;
    if (off < page->obj_start || off >= PAGE_SIZE) return false;
    return ((off - page->obj_start) % page->obj_size) == 0;
}

static bool slab_page_in_cache(const SlabCache *cache, const SlabPage *target) {
    for (const SlabPage *p = cache->pages; p; p = p->next)
        if (p == target) return true;
    return false;
}

// Walk the free-list to catch double-frees before they corrupt the allocator.
static bool slab_ptr_is_free_in_page(const SlabPage *page, const void *ptr) {
    const void *it = page->freelist;
    uint16_t seen = 0;

    while (it && seen < page->total_count) {
        if (it == ptr) return true;
        if (!slab_ptr_belongs_to_page(page, it)) return false;
        it = *(void * const *)it;
        seen++;
    }
    return false;
}

static SlabPage *slab_new_page(int cls) {
    uint16_t obj_size = slab_sizes[cls];
    SlabPage *page = (SlabPage *)_kmalloc_locked(PAGE_SIZE, PAGE_SIZE);
    if (!page) return NULL;

    size_t   hdr_end   = sizeof(SlabPage);
    size_t   obj_start = (hdr_end + obj_size - 1) & ~(size_t)(obj_size - 1);
    if (obj_start >= PAGE_SIZE) { _kfree_locked(page); return NULL; }
    uint16_t count     = (uint16_t)((PAGE_SIZE - obj_start) / obj_size);
    if (!count) { _kfree_locked(page); return NULL; }

    page->magic       = SLAB_PAGE_MAGIC;
    page->magic_inv   = SLAB_PAGE_MAGIC_INV;
    page->obj_size    = obj_size;
    page->free_count  = count;
    page->total_count = count;
    page->obj_start   = (uint16_t)obj_start;
    page->class_idx   = (uint16_t)cls;
    page->_pad        = 0;
    page->next        = NULL;

    uintptr_t base = (uintptr_t)page + obj_start;
    for (uint16_t k = 0; k < count - 1; k++)
        *(void **)(base + (size_t)k * obj_size) = (void *)(base + (size_t)(k + 1) * obj_size);
    *(void **)(base + (size_t)(count - 1) * obj_size) = NULL;
    page->freelist = (void *)base;
    return page;
}

// Locate the owning SlabPage by masking the lower 12 bits of ptr (pages are PAGE_SIZE-aligned).
// Runs a battery of header checks before trusting the page, guarding against wild pointers.
static inline bool slab_owns(void *ptr, SlabPage **out) {
    if (!ptr) return false;

    uintptr_t uptr = (uintptr_t)ptr;
    SlabPage *page = (SlabPage *)(uptr & ~(PAGE_SIZE - 1));

    if (page->magic != SLAB_PAGE_MAGIC) return false;
    if (page->magic_inv != SLAB_PAGE_MAGIC_INV) return false;
    if (!page->obj_size || !page->total_count) return false;
    if (page->obj_start < sizeof(SlabPage) || page->obj_start >= PAGE_SIZE) return false;
    if (page->class_idx >= SLAB_CLASSES) return false;
    if (slab_sizes[page->class_idx] != page->obj_size) return false;
    if (page->free_count > page->total_count) return false;

    uint16_t expected = (uint16_t)((PAGE_SIZE - page->obj_start) / page->obj_size);
    if (expected != page->total_count) return false;

    if (page->freelist && !slab_ptr_belongs_to_page(page, page->freelist)) return false;
    // slab_page_in_cache is checked last. It walks the linked list, so the cheap magic/bounds
    // checks above reject the vast majority of wild pointers before reaching it.
    if (!slab_page_in_cache(&slab_caches[page->class_idx], page)) return false;

    if (!slab_ptr_belongs_to_page(page, ptr)) return false;

    *out = page;
    return true;
}

static void *slab_alloc(int cls) {
    SlabCache *cache = &slab_caches[cls];

    SlabPage *page = cache->pages;
    while (page && page->free_count == 0)
        page = page->next;

    if (!page) {
        page = slab_new_page(cls);
        if (!page) return NULL;
        page->next   = cache->pages;
        cache->pages = page;
    }

    void *obj = page->freelist;

    // Freelist head must be a kernel higher-half address. Treat anything below the conservative
    // threshold 0xFFFF000000000000 as corruption (canonical boundary is 0xFFFF800000000000).
    if ((uintptr_t)obj < 0xFFFF000000000000ULL) {
        char b[17]; extern void itoa_hex(uint64_t, char *);
        serial_write("[SLAB] corrupt freelist cls=");
        itoa_hex((uint64_t)cls, b); serial_write(b);
        serial_write(" page="); itoa_hex((uint64_t)page, b); serial_write(b);
        serial_write(" fl=");   itoa_hex((uint64_t)obj, b);  serial_write(b);
        serial_write("\n");

        // Remove the corrupted page from the list to avoid hitting it again
        if (cache->pages == page) {
            cache->pages = page->next;
        } else {
            SlabPage *prev = cache->pages;
            while (prev && prev->next != page) prev = prev->next;
            if (prev) prev->next = page->next;
        }

        page->free_count = 0;
        page->freelist   = NULL;
        page->next       = NULL; // Isolate it

        page = slab_new_page(cls);
        if (!page) return NULL;
        page->next   = cache->pages;
        cache->pages = page;
        obj = page->freelist;
    }

    page->freelist = *(void **)obj;
    page->free_count--;
    cache->total_allocs++;
    slab_total_allocs++;

    memset(obj, 0, slab_sizes[cls]);
    return obj;
}

static void slab_free(void *ptr) {
    SlabPage *page;
    if (!slab_owns(ptr, &page)) return;
    // Fast over-free guard: if the page is already completely free there is nothing valid to free.
    if (page->free_count >= page->total_count) return;
    if (slab_ptr_is_free_in_page(page, ptr)) return;

    *(void **)ptr  = page->freelist;
    page->freelist = ptr;
    page->free_count++;

    int cls = slab_class_for_size(page->obj_size);
    if (cls >= 0) {
        slab_caches[cls].total_frees++;
        slab_total_frees++;
    }
}

void memory_manager_init_from_memmap(struct limine_memmap_response *memmap) {
    if (initialized || !memmap) return;

    memset(_bootstrap_blocks, 0, sizeof(_bootstrap_blocks));
    block_list      = _bootstrap_blocks;
    block_capacity  = BLOCK_LIST_INITIAL_CAPACITY;
    block_count     = 0;
    on_heap         = false;
    total_allocated = peak_allocated = allocation_counter = 0;
    memory_pool_size = 0;
    memset(slab_caches, 0, sizeof(slab_caches));
    slab_total_allocs = slab_total_frees = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base, size = e->length;
        // Skip the first 1 MiB. Real-mode IVT, BIOS data, and legacy ROM regions live here.
        if (base < 0x100000) {
            if (base + size <= 0x100000) continue;
            size -= 0x100000 - base;
            base  = 0x100000;
        }
        if (size < PAGE_SIZE) continue;
        if (block_count >= block_capacity) break;

        block_list[block_count++] = (MemBlock){
            .address = (void *)p2v(base), .size = size, 
        };
        memory_pool_size += size;
    }

    sort_block_list();
    initialized = true;

    serial_write("[MEM] Total usable memory: ");
    serial_write_num((uint32_t)(memory_pool_size / 1024 / 1024));
    serial_write(" MB\n");
}

// Routes small (<= 512 B, alignment <= 8) requests to the slab allocator; everything else uses the block list.
void *kmalloc_aligned(size_t size, size_t alignment) {
    if (!initialized || size == 0) return NULL;

    uint64_t rflags = spinlock_acquire_irqsave(&mm_lock);
    void *ptr;

    if (alignment <= 8) {
        int cls = slab_class_for_size(size);
        if (cls >= 0) {
            ptr = slab_alloc(cls);
            spinlock_release_irqrestore(&mm_lock, rflags);
            return ptr;
        }
    }

    ptr = _kmalloc_locked(size, alignment);
    spinlock_release_irqrestore(&mm_lock, rflags);
    return ptr;
}

void *kmalloc(size_t size) {
    return kmalloc_aligned(size, 8);
}

// kcalloc ensures memory is zeroed, which is critical for many kernel and library 
// structures (like lwIP PCBs) that assume a null-initialized state.
void *kcalloc(size_t n, size_t size) {
    size_t total = n * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr || !initialized) return;
    uint64_t rflags = spinlock_acquire_irqsave(&mm_lock);
    SlabPage *page;
    if (slab_owns(ptr, &page))
        slab_free(ptr);
    else
        _kfree_locked(ptr);
    spinlock_release_irqrestore(&mm_lock, rflags);
}


void *krealloc(void *ptr, size_t new_size) {
    if (new_size == 0) { kfree(ptr); return NULL; }
    if (!ptr)           return kmalloc(new_size);

    new_size = (new_size + 7) & ~7ULL;

    uint64_t rflags = spinlock_acquire_irqsave(&mm_lock);

    size_t old_size = 0;
    SlabPage *page = NULL;
    int block_idx = -1;
    bool is_slab = slab_owns(ptr, &page);
    
    if (is_slab) {
        old_size = page->obj_size;
    } else {
        for (int i = 0; i < block_count; i++) {
            if (block_list[i].allocated && block_list[i].address == ptr) {
                old_size = block_list[i].size;
                block_idx = i;
                break;
            }
        }
    }

    if (!old_size) { spinlock_release_irqrestore(&mm_lock, rflags); return NULL; }

    // Shrink-in-place and migration logic
    if (old_size > new_size) {
        if (is_slab) {
            int new_cls = slab_class_for_size(new_size);
            // If the shrink requirement pushes the allocation into a smaller slab class,
            // fall through the check to trigger standard copy-migration to free the bigger slot.
            if (new_cls < 0 || slab_sizes[new_cls] >= page->obj_size) {
                spinlock_release_irqrestore(&mm_lock, rflags);
                return ptr;
            }
        } else if (block_idx >= 0) {
            // Block Allocator: Shrink dynamic blocks if threshold >= 32 bytes to prevent micro-fragmentation.
            size_t diff = old_size - new_size;
            if (diff >= 32) {
                block_list[block_idx].size = new_size;
                void *tail_addr = (void *)((uintptr_t)ptr + new_size);
                
                if (insert_block_at(block_idx + 1, tail_addr, diff, false, 0)) {
                    total_allocated -= diff;
                    
                    int f_idx = block_idx + 1;
                    if (f_idx + 1 < block_count && !block_list[f_idx + 1].allocated &&
                        (uintptr_t)block_list[f_idx].address + block_list[f_idx].size ==
                        (uintptr_t)block_list[f_idx + 1].address) {
                        block_list[f_idx].size += block_list[f_idx + 1].size;
                        remove_block_at(f_idx + 1);
                    }
                } else {
                    block_list[block_idx].size = old_size; 
                }
            }
            spinlock_release_irqrestore(&mm_lock, rflags);
            return ptr;
        }
    }
    
    if (old_size == new_size) {
        spinlock_release_irqrestore(&mm_lock, rflags);
        return ptr;
    }

    int cls = slab_class_for_size(new_size);
    void *np = (cls >= 0) ? slab_alloc(cls) : _kmalloc_locked(new_size, 8);
    if (!np) { spinlock_release_irqrestore(&mm_lock, rflags); return NULL; }

    // Hold the lock across both the new alloc and the free of the old pointer
    // to keep the operation atomic (no other CPU can observe a partial realloc).
    mem_memmove(np, ptr, old_size);
    if (slab_owns(ptr, &page))
        slab_free(ptr);
    else
        _kfree_locked(ptr);

    spinlock_release_irqrestore(&mm_lock, rflags);
    return np;
}

MemStats memory_get_stats(void) {
    MemStats s = {0};
    s.total_memory     = memory_pool_size;
    s.used_memory      = total_allocated;
    s.available_memory = memory_pool_size - total_allocated;
    s.peak_memory_used = peak_allocated;
    s.smallest_free_block = memory_pool_size;

    for (int i = 0; i < block_count; i++) {
        if (block_list[i].allocated) {
            s.allocated_blocks++;
        } else {
            s.free_blocks++;
            if (block_list[i].size > s.largest_free_block)
                s.largest_free_block = block_list[i].size;
            if (block_list[i].size < s.smallest_free_block)
                s.smallest_free_block = block_list[i].size;
        }
    }
    if (!s.free_blocks) s.smallest_free_block = 0;

    s.fragmentation_percent = calculate_fragmentation();
    s.slab_allocs           = slab_total_allocs;
    s.slab_frees            = slab_total_frees;
    return s;
}

bool mm_is_heap_address(void *ptr) {
    if (!ptr || !initialized) return false;
    
    uint64_t rflags = spinlock_acquire_irqsave(&mm_lock);
    
    // Check slab allocator first
    SlabPage *page;
    if (slab_owns(ptr, &page)) {
        spinlock_release_irqrestore(&mm_lock, rflags);
        return true;
    }
    
    // Check block list
    for (int i = 0; i < block_count; i++) {
        uintptr_t base = (uintptr_t)block_list[i].address;
        size_t size = block_list[i].size;
        if ((uintptr_t)ptr >= base && (uintptr_t)ptr < base + size) {
            spinlock_release_irqrestore(&mm_lock, rflags);
            return true;
        }
    }
    spinlock_release_irqrestore(&mm_lock, rflags);
    return false;
}
