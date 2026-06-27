#ifndef SHM_H
#define SHM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SHM_MAX_PAGES 2048 // Supports up to 8MB shared memory

typedef struct shm_segment {
    char name[64];
    uint64_t phys_pages[SHM_MAX_PAGES]; 
    uint32_t page_count;
    uint32_t size;
    int ref_count;
    struct shm_segment *next;
} shm_segment_t;

shm_segment_t* shm_get_or_create(const char *name);
void shm_ref(shm_segment_t *seg);
void shm_unref(shm_segment_t *seg);
int shm_allocate(shm_segment_t *seg, size_t size);
void shm_unlink(const char *name);
bool shm_exists(const char *name);

#endif
