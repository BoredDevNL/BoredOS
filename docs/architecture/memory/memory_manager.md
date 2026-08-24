# Slab Allocator & Kernel Heap

Source: [`mem/slab.c`](../../mem/slab.c), [`mem/slab.h`](../../mem/slab.h)

The slab allocator handles kernel object caches and dynamic heap allocations (`kmalloc`, `kfree`).

## Allocator Structure

The allocator has two modes:
1. **Custom Object Caches**: Subsystems create dedicated caches (`slab_cache_t`) for fixed-size structs with optional constructors and destructors.
2. **General Heap**: `kmalloc` uses geometric slab classes for small requests, and delegates directly to the PMM for large requests.

Each slab cache tracks:
- Full slabs: All slots in use.
- Partial slabs: Some slots in use, free slots tracked via an intrusive free list.
- Empty slabs: All slots free, eligible for freeing by `slab_cache_reap()`.

## General Heap Allocation

`kmalloc` routes allocations through fixed size classes:
- Sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, and 8192 bytes.
- Requests are rounded up to the nearest class size.
- Allocations over 8192 bytes bypass slab caches and allocate directly from the PMM via `pmm_alloc_order()` with `PAGE_FLAG_KMALLOC_LARGE`.

### Alignment

- Default minimum alignment is 16 bytes (`SLAB_MIN_ALIGN`).
- `kmalloc_aligned()` supports power-of-two alignments such as 32-byte AVX (`SLAB_AVX_ALIGN`) and 4096-byte page alignment.

## Custom Slab Caches

Subsystems can create dedicated caches:

### Cache API

- `slab_cache_create(name, obj_size, align, flags)`: Creates a cache.
- `slab_cache_create_with_ctor(name, obj_size, align, flags, ctor, dtor)`: Creates a cache with ctor/dtor callbacks.
- `slab_cache_alloc(cache)`: Allocates an object from the cache.
- `slab_cache_free(cache, obj)`: Returns an object to the cache.
- `slab_cache_reap(cache)`: Frees empty slab pages back to the PMM.
- `slab_cache_destroy(cache)`: Destroys the cache and frees its pages.

### Flags

- `SLAB_FLAG_NONE`: Default.
- `SLAB_FLAG_POISON`: Writes `0x5A` on allocation and `0xDE` on free to catch memory errors.
- `SLAB_FLAG_ZERO`: Zeroes object memory on allocation.
- `SLAB_FLAG_PER_CPU`: Uses per-CPU magazine caches.
- `SLAB_FLAG_RCU`: Delays slab page freeing for RCU read grace periods.

## Heap API

| Function | Signature | Description |
| :--- | :--- | :--- |
| `kmalloc` | `void *kmalloc(size_t size)` | Allocates memory from slab class or PMM. |
| `kzalloc` | `void *kzalloc(size_t size)` | Allocates zeroed memory. |
| `kcalloc` | `void *kcalloc(size_t n, size_t size)` | Allocates zeroed array. |
| `kmalloc_aligned` | `void *kmalloc_aligned(size_t size, size_t alignment)` | Allocates aligned memory. |
| `krealloc` | `void *krealloc(void *ptr, size_t new_size)` | Resizes an allocated buffer. |
| `kfree` | `void kfree(void *ptr)` | Frees memory allocated by `kmalloc`. |
| `kfree_null` | `kfree_null(ptr)` | Frees `ptr` and clears the pointer. |
| `mm_is_heap_address` | `bool mm_is_heap_address(void *ptr)` | Checks if an address belongs to the heap. |

All slab caches are synchronized with spinlocks.
