// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "slab.h"
#include "pmm.h"
#include "spinlock.h"
#include "platform.h"
#include "smp.h"
#include <string.h>

#define SLAB_MAX_MAGAZINE_SIZE   128
#define SLAB_BOOTSTRAP_MAGAZINES 128
#define SLAB_BOOTSTRAP_CACHES    32
#define SLAB_BUFCTL_END          0xFFFF
#define SLAB_MAX_CPUS            64

typedef struct slab_magazine {
    struct slab_magazine *next;
    uint32_t count;
    uint32_t capacity;
    void *objects[];
} slab_magazine_t;

typedef struct {
    slab_magazine_t hdr;
    void *objects[SLAB_MAX_MAGAZINE_SIZE];
} slab_bootstrap_mag_t;

typedef struct {
    slab_magazine_t *loaded;
    slab_magazine_t *previous;
} __attribute__((aligned(64))) slab_cpu_cache_t;

typedef struct {
    spinlock_t lock;
    slab_magazine_t *full_magazines;
    slab_magazine_t *empty_magazines;
    size_t full_count;
    size_t empty_count;
} slab_depot_t;

typedef struct slab {
    struct slab *prev;
    struct slab *next;
    struct slab_cache *cache;
    void *memory_base;
    size_t inuse_count;
    size_t total_count;
    size_t color_offset;
    bool is_off_slab;

    void *free_list;
    uint16_t bufctl_head;
    uint16_t bufctl[];
} slab_t;

struct slab_cache {
    char name[32];
    size_t obj_size;
    size_t align;
    uint32_t flags;
    uint8_t slab_order;
    size_t objs_per_slab;
    uint32_t magazine_size;
    bool off_slab;

    size_t color_max;
    size_t color_next;
    size_t color_align;

    slab_ctor_t ctor;
    slab_dtor_t dtor;

    spinlock_t lock;
    slab_t *slabs_partial;
    slab_t *slabs_full;
    slab_t *slabs_empty;
    size_t empty_slab_count;

    slab_depot_t depot;
    slab_cpu_cache_t cpu_caches[SLAB_MAX_CPUS];
};

static slab_cache_t bootstrap_caches[SLAB_BOOTSTRAP_CACHES];
static size_t bootstrap_cache_count = 0;

static slab_bootstrap_mag_t bootstrap_magazines[SLAB_BOOTSTRAP_MAGAZINES];
static size_t bootstrap_mag_count = 0;

static slab_cache_t *magazine_cache = NULL;
static slab_cache_t *slab_meta_cache = NULL;

static const size_t kmalloc_sizes[] = {16, 32, 64, 128, 192, 256, 512, 1024, 2048};
#define KMALLOC_CACHE_COUNT (sizeof(kmalloc_sizes) / sizeof(kmalloc_sizes[0]))
static slab_cache_t *kmalloc_caches[KMALLOC_CACHE_COUNT];

static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static inline uint8_t pages_to_order(size_t count) {
    if (count <= 1) return 0;
    if (count > (1UL << (PMM_MAX_ORDER - 1))) return PMM_MAX_ORDER;
    return (uint8_t)(64 - __builtin_clzll(count - 1));
}

static inline size_t align_up(size_t val, size_t align) {
    if (align <= 1) return val;
    return (val + align - 1) & ~(align - 1);
}

static inline void slab_zero_fast(void *dest, size_t size) {
    if (!dest || size == 0) return;

    uint8_t *d = (uint8_t *)dest;
    while (size && ((uintptr_t)d & 7)) {
        *d++ = 0;
        size--;
    }

    uint64_t *d64 = (uint64_t *)d;
    while (size >= 64) {
        d64[0] = 0; d64[1] = 0; d64[2] = 0; d64[3] = 0;
        d64[4] = 0; d64[5] = 0; d64[6] = 0; d64[7] = 0;
        d64 += 8;
        size -= 64;
    }

    while (size >= 8) {
        *d64++ = 0;
        size -= 8;
    }

    d = (uint8_t *)d64;
    while (size > 0) {
        *d++ = 0;
        size--;
    }
}

static inline void slab_copy_fast(void *dest, const void *src, size_t size) {
    if (!dest || !src || size == 0) return;

    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    while (size && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        size--;
    }

    if (((uintptr_t)s & 7) == 0) {
        uint64_t *d64 = (uint64_t *)d;
        const uint64_t *s64 = (const uint64_t *)s;
        while (size >= 64) {
            d64[0] = s64[0]; d64[1] = s64[1]; d64[2] = s64[2]; d64[3] = s64[3];
            d64[4] = s64[4]; d64[5] = s64[5]; d64[6] = s64[6]; d64[7] = s64[7];
            d64 += 8; s64 += 8;
            size -= 64;
        }
        while (size >= 8) {
            *d64++ = *s64++;
            size -= 8;
        }
        d = (uint8_t *)d64;
        s = (const uint8_t *)s64;
    }

    while (size > 0) {
        *d++ = *s++;
        size--;
    }
}

static slab_magazine_t *mag_alloc(uint32_t capacity) {
    if (magazine_cache) {
        slab_magazine_t *m = (slab_magazine_t *)slab_cache_alloc(magazine_cache);
        if (m) {
            m->next = NULL;
            m->count = 0;
            m->capacity = capacity;
            return m;
        }
    }
    if (bootstrap_mag_count < SLAB_BOOTSTRAP_MAGAZINES) {
        slab_magazine_t *m = (slab_magazine_t *)&bootstrap_magazines[bootstrap_mag_count++];
        m->next = NULL;
        m->count = 0;
        m->capacity = capacity;
        return m;
    }
    return NULL;
}

static void mag_free(slab_magazine_t *mag) {
    if (!mag) return;
    uintptr_t addr = (uintptr_t)mag;
    uintptr_t b_start = (uintptr_t)&bootstrap_magazines[0];
    uintptr_t b_end = (uintptr_t)&bootstrap_magazines[SLAB_BOOTSTRAP_MAGAZINES];
    if (addr >= b_start && addr < b_end) {
        return;
    }
    if (magazine_cache) {
        slab_cache_free(magazine_cache, mag);
    }
}

static void mag_depot_put_full(slab_depot_t *depot, slab_magazine_t *mag) {
    uint64_t flags = spinlock_acquire_irqsave(&depot->lock);
    mag->next = depot->full_magazines;
    depot->full_magazines = mag;
    depot->full_count++;
    spinlock_release_irqrestore(&depot->lock, flags);
}

static slab_magazine_t *mag_depot_get_full(slab_depot_t *depot) {
    uint64_t flags = spinlock_acquire_irqsave(&depot->lock);
    slab_magazine_t *mag = depot->full_magazines;
    if (mag) {
        depot->full_magazines = mag->next;
        mag->next = NULL;
        depot->full_count--;
    }
    spinlock_release_irqrestore(&depot->lock, flags);
    return mag;
}

static void mag_depot_put_empty(slab_depot_t *depot, slab_magazine_t *mag) {
    uint64_t flags = spinlock_acquire_irqsave(&depot->lock);
    mag->next = depot->empty_magazines;
    depot->empty_magazines = mag;
    depot->empty_count++;
    spinlock_release_irqrestore(&depot->lock, flags);
}

static slab_magazine_t *mag_depot_get_empty(slab_depot_t *depot, uint32_t capacity) {
    uint64_t flags = spinlock_acquire_irqsave(&depot->lock);
    slab_magazine_t *mag = depot->empty_magazines;
    if (mag) {
        depot->empty_magazines = mag->next;
        mag->next = NULL;
        depot->empty_count--;
    }
    spinlock_release_irqrestore(&depot->lock, flags);

    if (!mag) {
        mag = mag_alloc(capacity);
    }
    return mag;
}

static inline void list_add_slab(slab_t **head, slab_t *slab) {
    slab->prev = NULL;
    slab->next = *head;
    if (*head) (*head)->prev = slab;
    *head = slab;
}

static inline void list_remove_slab(slab_t **head, slab_t *slab) {
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else if (*head == slab) {
        *head = slab->next;
    }
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    slab->prev = NULL;
    slab->next = NULL;
}

static slab_t *slab_create_page(slab_cache_t *cache) {
    page_t *head_page = pmm_alloc_order(cache->slab_order, PAGE_FLAG_KERNEL);
    if (!head_page) return NULL;

    uintptr_t head_pfn = pmm_page_to_paddr(head_page) >> PMM_PAGE_SHIFT;
    void *page_vaddr = (void *)p2v(head_pfn << PMM_PAGE_SHIFT);
    size_t slab_bytes = (1UL << cache->slab_order) * PMM_PAGE_SIZE;

    slab_t *slab = NULL;
    if (cache->off_slab) {
        if (slab_meta_cache) {
            slab = (slab_t *)slab_cache_alloc(slab_meta_cache);
        } else if (bootstrap_cache_count < SLAB_BOOTSTRAP_CACHES) {
            slab = (slab_t *)&bootstrap_caches[bootstrap_cache_count++];
        }
        if (!slab) {
            pmm_free_order(head_page, cache->slab_order);
            return NULL;
        }
        slab->is_off_slab = true;
    } else {
        size_t header_bytes = sizeof(slab_t) + (cache->ctor ? cache->objs_per_slab * sizeof(uint16_t) : 0);
        header_bytes = align_up(header_bytes, 16);
        slab = (slab_t *)((uintptr_t)page_vaddr + slab_bytes - header_bytes);
        slab->is_off_slab = false;
    }

    for (size_t i = 0; i < (1UL << cache->slab_order); i++) {
        page_t *p = pmm_paddr_to_page((head_pfn + i) << PMM_PAGE_SHIFT);
        if (p) {
            p->flags |= PAGE_FLAG_SLAB;
            p->mapping = slab;
        }
    }

    slab->cache = cache;
    slab->inuse_count = 0;
    slab->total_count = cache->objs_per_slab;
    slab->color_offset = cache->color_next;
    slab->memory_base = (void *)((uintptr_t)page_vaddr + slab->color_offset);

    if (cache->color_max >= cache->color_align) {
        cache->color_next = (cache->color_next + cache->color_align) % (cache->color_max + 1);
    }

    if (cache->ctor == NULL) {
        slab->free_list = NULL;
        for (size_t i = 0; i < cache->objs_per_slab; i++) {
            void *obj = (void *)((uintptr_t)slab->memory_base + i * cache->obj_size);
            *(void **)obj = slab->free_list;
            slab->free_list = obj;
        }
    } else {
        slab->bufctl_head = 0;
        for (size_t i = 0; i < cache->objs_per_slab; i++) {
            slab->bufctl[i] = (i + 1 < cache->objs_per_slab) ? (uint16_t)(i + 1) : SLAB_BUFCTL_END;
            void *obj = (void *)((uintptr_t)slab->memory_base + i * cache->obj_size);
            cache->ctor(obj, cache->obj_size);
        }
    }

    return slab;
}

static void slab_destroy_page(slab_cache_t *cache, slab_t *slab) {
    if (!slab || !cache) return;

    if (cache->dtor) {
        for (size_t i = 0; i < cache->objs_per_slab; i++) {
            void *obj = (void *)((uintptr_t)slab->memory_base + i * cache->obj_size);
            cache->dtor(obj, cache->obj_size);
        }
    }

    page_t *head_page = pmm_vaddr_to_page(slab->memory_base);
    if (head_page) {
        uintptr_t head_pfn = pmm_page_to_paddr(head_page) >> PMM_PAGE_SHIFT;
        for (size_t i = 0; i < (1UL << cache->slab_order); i++) {
            page_t *p = pmm_paddr_to_page((head_pfn + i) << PMM_PAGE_SHIFT);
            if (p) {
                p->flags &= ~PAGE_FLAG_SLAB;
                p->mapping = NULL;
            }
        }
        pmm_free_order(head_page, cache->slab_order);
    }

    if (slab->is_off_slab && slab_meta_cache) {
        slab_cache_free(slab_meta_cache, slab);
    }
}

slab_cache_t *slab_cache_create_with_ctor(const char *name, size_t obj_size, size_t align, uint32_t flags,
                                          slab_ctor_t ctor, slab_dtor_t dtor) {
    if (obj_size == 0) return NULL;
    if (align < SLAB_MIN_ALIGN) align = SLAB_MIN_ALIGN;

    size_t aligned_size = align_up(obj_size, align);
    if (aligned_size < sizeof(void *)) aligned_size = sizeof(void *);

    slab_cache_t *cache = NULL;
    if (bootstrap_cache_count < SLAB_BOOTSTRAP_CACHES) {
        cache = &bootstrap_caches[bootstrap_cache_count++];
    } else {
        cache = (slab_cache_t *)kmalloc(sizeof(slab_cache_t));
    }
    if (!cache) return NULL;

    memset(cache, 0, sizeof(slab_cache_t));
    strncpy(cache->name, name ? name : "unnamed", sizeof(cache->name) - 1);
    cache->obj_size = aligned_size;
    cache->align = align;
    cache->flags = flags;
    cache->ctor = ctor;
    cache->dtor = dtor;
    cache->lock = SPINLOCK_INIT;
    cache->depot.lock = SPINLOCK_INIT;

    if (aligned_size <= 64) {
        cache->magazine_size = 64;
    } else if (aligned_size <= 512) {
        cache->magazine_size = 32;
    } else {
        cache->magazine_size = 8;
    }

    if (aligned_size >= 1024) {
        cache->off_slab = true;
        cache->slab_order = 0;
        cache->objs_per_slab = PMM_PAGE_SIZE / aligned_size;
        cache->color_max = PMM_PAGE_SIZE % aligned_size;
    } else {
        cache->off_slab = false;
        cache->slab_order = 0;
        size_t approx_objs = PMM_PAGE_SIZE / aligned_size;
        size_t hdr_bytes = sizeof(slab_t) + (ctor ? approx_objs * sizeof(uint16_t) : 0);
        hdr_bytes = align_up(hdr_bytes, 16);
        size_t slack = PMM_PAGE_SIZE - hdr_bytes;
        cache->color_max = slack % aligned_size;
        cache->objs_per_slab = (slack - cache->color_max) / aligned_size;
    }

    cache->color_align = 64;
    cache->color_next = 0;

    for (size_t c = 0; c < SLAB_MAX_CPUS; c++) {
        cache->cpu_caches[c].loaded = mag_alloc(cache->magazine_size);
        cache->cpu_caches[c].previous = mag_alloc(cache->magazine_size);
    }

    return cache;
}

slab_cache_t *slab_cache_create(const char *name, size_t obj_size, size_t align, uint32_t flags) {
    return slab_cache_create_with_ctor(name, obj_size, align, flags, NULL, NULL);
}

void slab_cache_destroy(slab_cache_t *cache) {
    if (!cache) return;
    slab_cache_reap(cache);
}

static void *slab_alloc_from_lists(slab_cache_t *cache) {
    uint64_t flags = spinlock_acquire_irqsave(&cache->lock);

    slab_t *slab = cache->slabs_partial;
    if (!slab) {
        slab = cache->slabs_empty;
        if (slab) {
            list_remove_slab(&cache->slabs_empty, slab);
            cache->empty_slab_count--;
            list_add_slab(&cache->slabs_partial, slab);
        }
    }

    if (!slab) {
        slab = slab_create_page(cache);
        if (!slab) {
            spinlock_release_irqrestore(&cache->lock, flags);
            return NULL;
        }
        list_add_slab(&cache->slabs_partial, slab);
    }

    void *obj = NULL;
    if (cache->ctor == NULL) {
        obj = slab->free_list;
        if (obj) {
            slab->free_list = *(void **)obj;
        }
    } else {
        if (slab->bufctl_head != SLAB_BUFCTL_END) {
            uint16_t idx = slab->bufctl_head;
            slab->bufctl_head = slab->bufctl[idx];
            obj = (void *)((uintptr_t)slab->memory_base + idx * cache->obj_size);
        }
    }

    if (obj) {
        slab->inuse_count++;
        if (slab->inuse_count == slab->total_count) {
            list_remove_slab(&cache->slabs_partial, slab);
            list_add_slab(&cache->slabs_full, slab);
        }
    }

    spinlock_release_irqrestore(&cache->lock, flags);
    return obj;
}

void *slab_cache_alloc(slab_cache_t *cache) {
    if (!cache) return NULL;

    uint32_t cpu_id = smp_this_cpu_id();
    if (cpu_id >= SLAB_MAX_CPUS) cpu_id = 0;

    slab_cpu_cache_t *cc = &cache->cpu_caches[cpu_id];
    uint64_t rflags = irq_save();

    slab_magazine_t *loaded = cc->loaded;
    if (loaded && loaded->count > 0) {
        loaded->count--;
        void *obj = loaded->objects[loaded->count];
        irq_restore(rflags);
        if (cache->flags & SLAB_FLAG_ZERO) slab_zero_fast(obj, cache->obj_size);
        return obj;
    }

    slab_magazine_t *prev = cc->previous;
    if (prev && prev->count > 0) {
        cc->loaded = prev;
        cc->previous = loaded;
        loaded = prev;
        loaded->count--;
        void *obj = loaded->objects[loaded->count];
        irq_restore(rflags);
        if (cache->flags & SLAB_FLAG_ZERO) slab_zero_fast(obj, cache->obj_size);
        return obj;
    }

    slab_magazine_t *full = mag_depot_get_full(&cache->depot);
    if (full) {
        if (loaded && loaded->count == 0) {
            mag_depot_put_empty(&cache->depot, loaded);
        }
        cc->loaded = full;
        loaded = full;
        loaded->count--;
        void *obj = loaded->objects[loaded->count];
        irq_restore(rflags);
        if (cache->flags & SLAB_FLAG_ZERO) slab_zero_fast(obj, cache->obj_size);
        return obj;
    }

    irq_restore(rflags);

    void *obj = slab_alloc_from_lists(cache);
    if (obj && (cache->flags & SLAB_FLAG_ZERO)) {
        slab_zero_fast(obj, cache->obj_size);
    }
    return obj;
}

static void slab_free_to_lists(slab_cache_t *cache, void *obj) {
    page_t *page = pmm_vaddr_to_page(obj);
    if (!page || !(page->flags & PAGE_FLAG_SLAB)) return;

    slab_t *slab = (slab_t *)page->mapping;
    if (!slab) return;

    uint64_t flags = spinlock_acquire_irqsave(&cache->lock);

    bool was_full = (slab->inuse_count == slab->total_count);

    if (cache->ctor == NULL) {
        *(void **)obj = slab->free_list;
        slab->free_list = obj;
    } else {
        size_t obj_idx = ((uintptr_t)obj - (uintptr_t)slab->memory_base) / cache->obj_size;
        slab->bufctl[obj_idx] = slab->bufctl_head;
        slab->bufctl_head = (uint16_t)obj_idx;
    }

    slab->inuse_count--;

    if (was_full) {
        list_remove_slab(&cache->slabs_full, slab);
        list_add_slab(&cache->slabs_partial, slab);
    }

    if (slab->inuse_count == 0) {
        list_remove_slab(&cache->slabs_partial, slab);
        if (cache->empty_slab_count >= 4) {
            spinlock_release_irqrestore(&cache->lock, flags);
            slab_destroy_page(cache, slab);
            return;
        } else {
            list_add_slab(&cache->slabs_empty, slab);
            cache->empty_slab_count++;
        }
    }

    spinlock_release_irqrestore(&cache->lock, flags);
}

void slab_cache_free(slab_cache_t *cache, void *obj) {
    if (!cache || !obj) return;

    uint32_t cpu_id = smp_this_cpu_id();
    if (cpu_id >= SLAB_MAX_CPUS) cpu_id = 0;

    slab_cpu_cache_t *cc = &cache->cpu_caches[cpu_id];
    uint64_t rflags = irq_save();

    slab_magazine_t *loaded = cc->loaded;
    if (loaded && loaded->count < loaded->capacity) {
        loaded->objects[loaded->count++] = obj;
        irq_restore(rflags);
        return;
    }

    slab_magazine_t *prev = cc->previous;
    if (prev && prev->count < prev->capacity) {
        cc->loaded = prev;
        cc->previous = loaded;
        prev->objects[prev->count++] = obj;
        irq_restore(rflags);
        return;
    }

    if (loaded && loaded->count >= loaded->capacity) {
        mag_depot_put_full(&cache->depot, loaded);
        cc->loaded = mag_depot_get_empty(&cache->depot, cache->magazine_size);
        if (cc->loaded) {
            cc->loaded->objects[cc->loaded->count++] = obj;
            irq_restore(rflags);
            return;
        }
    }

    irq_restore(rflags);
    slab_free_to_lists(cache, obj);
}

size_t slab_cache_reap(slab_cache_t *cache) {
    if (!cache) return 0;
    size_t reaped_pages = 0;

    slab_magazine_t *mag = mag_depot_get_full(&cache->depot);
    while (mag) {
        for (uint32_t i = 0; i < mag->count; i++) {
            slab_free_to_lists(cache, mag->objects[i]);
        }
        mag->count = 0;
        mag_depot_put_empty(&cache->depot, mag);
        mag = mag_depot_get_full(&cache->depot);
    }

    uint64_t flags = spinlock_acquire_irqsave(&cache->lock);
    while (cache->slabs_empty) {
        slab_t *s = cache->slabs_empty;
        list_remove_slab(&cache->slabs_empty, s);
        cache->empty_slab_count--;
        spinlock_release_irqrestore(&cache->lock, flags);

        slab_destroy_page(cache, s);
        reaped_pages += (1UL << cache->slab_order);

        flags = spinlock_acquire_irqsave(&cache->lock);
    }
    spinlock_release_irqrestore(&cache->lock, flags);
    return reaped_pages;
}

void slab_init(void) {
    magazine_cache = slab_cache_create("slab_magazine", sizeof(slab_bootstrap_mag_t), 16, SLAB_FLAG_NONE);
    slab_meta_cache = slab_cache_create("slab_metadata", sizeof(slab_t) + 16 * sizeof(uint16_t), 16, SLAB_FLAG_NONE);

    char name_buf[32];
    for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
        size_t sz = kmalloc_sizes[i];
        strcpy(name_buf, "kmalloc-");
        if (sz >= 1000) {
            name_buf[8] = '0' + (sz / 1000);
            name_buf[9] = '0' + ((sz % 1000) / 100);
            name_buf[10] = '0' + ((sz % 100) / 10);
            name_buf[11] = '0' + (sz % 10);
            name_buf[12] = '\0';
        } else if (sz >= 100) {
            name_buf[8] = '0' + (sz / 100);
            name_buf[9] = '0' + ((sz % 100) / 10);
            name_buf[10] = '0' + (sz % 10);
            name_buf[11] = '\0';
        } else {
            name_buf[8] = '0' + (sz / 10);
            name_buf[9] = '0' + (sz % 10);
            name_buf[10] = '\0';
        }
        kmalloc_caches[i] = slab_cache_create(name_buf, sz, 16, SLAB_FLAG_NONE);
    }
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0) return NULL;
    if (alignment < SLAB_MIN_ALIGN) alignment = SLAB_MIN_ALIGN;

    if (size <= 2048 && alignment <= 64) {
        for (size_t i = 0; i < KMALLOC_CACHE_COUNT; i++) {
            if (kmalloc_sizes[i] >= size) {
                return slab_cache_alloc(kmalloc_caches[i]);
            }
        }
    }

    size_t page_count = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint8_t order = pages_to_order(page_count);
    if (order >= PMM_MAX_ORDER) return NULL;

    page_t *page = pmm_alloc_order(order, PAGE_FLAG_KERNEL);
    if (!page) return NULL;

    uintptr_t head_pfn = pmm_page_to_paddr(page) >> PMM_PAGE_SHIFT;
    for (size_t i = 0; i < (1UL << order); i++) {
        page_t *p = pmm_paddr_to_page((head_pfn + i) << PMM_PAGE_SHIFT);
        if (p) {
            p->flags |= PAGE_FLAG_KMALLOC_LARGE;
            p->order = order;
        }
    }

    return (void *)p2v(head_pfn << PMM_PAGE_SHIFT);
}

void *kmalloc(size_t size) {
    return kmalloc_aligned(size, SLAB_MIN_ALIGN);
}

void *kzalloc(size_t size) {
    void *ptr = kmalloc(size);
    if (ptr) slab_zero_fast(ptr, size);
    return ptr;
}

void *kcalloc(size_t n, size_t size) {
    if (n != 0 && size > SIZE_MAX / n) return NULL;
    size_t total = n * size;
    void *ptr = kmalloc(total);
    if (ptr) slab_zero_fast(ptr, total);
    return ptr;
}

void kfree(void *ptr) {
    if (!ptr) return;

    page_t *page = pmm_vaddr_to_page(ptr);
    if (!page) return;

    if (page->flags & PAGE_FLAG_KMALLOC_LARGE) {
        uintptr_t head_pfn = pmm_page_to_paddr(page) >> PMM_PAGE_SHIFT;
        uint8_t order = page->order;
        for (size_t i = 0; i < (1UL << order); i++) {
            page_t *p = pmm_paddr_to_page((head_pfn + i) << PMM_PAGE_SHIFT);
            if (p) {
                p->flags &= ~PAGE_FLAG_KMALLOC_LARGE;
            }
        }
        pmm_free_order(page, order);
        return;
    }

    if (page->flags & PAGE_FLAG_SLAB) {
        slab_t *slab = (slab_t *)page->mapping;
        if (slab && slab->cache) {
            slab_cache_free(slab->cache, ptr);
        }
        return;
    }
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    page_t *page = pmm_vaddr_to_page(ptr);
    if (!page) return NULL;

    size_t old_size = 0;
    if (page->flags & PAGE_FLAG_KMALLOC_LARGE) {
        old_size = (1UL << page->order) * PMM_PAGE_SIZE;
    } else if (page->flags & PAGE_FLAG_SLAB) {
        slab_t *slab = (slab_t *)page->mapping;
        if (slab && slab->cache) {
            old_size = slab->cache->obj_size;
        }
    }

    if (old_size >= new_size && old_size <= new_size * 2) {
        return ptr;
    }

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;

    size_t copy_size = (old_size < new_size) ? old_size : new_size;
    if (copy_size > 0) {
        slab_copy_fast(new_ptr, ptr, copy_size);
    }

    kfree(ptr);
    return new_ptr;
}

bool mm_is_heap_address(void *ptr) {
    if (!ptr) return false;
    page_t *page = pmm_vaddr_to_page(ptr);
    return page != NULL;
}
