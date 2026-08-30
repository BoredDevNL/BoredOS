# Memory Architecture

BoredOS memory management consists of physical memory allocation, page table management, virtual memory area tracking, per-process virtual memory management, slab allocation, and a page cache.

## Subsystems

- [PMM](#physical-memory-manager-pmm): Physical page frame allocator.
- [MMU](#mmu-driver): x86_64 4-level page table manipulation and TLB shootdown.
- [VMA](#virtual-memory-areas-vma): Per-process virtual memory ranges tracked with an RB-tree and linked list.
- [VMM](#virtual-memory-manager-vmm): Address spaces, demand paging, COW, and syscall backends (`mmap`, `brk`).
- [Slab Allocator](memory_manager.md): Kernel object caches and `kmalloc`/`kfree`.
- [Page Cache](pagecache.md): Page cache for file and block device backing.

---

## Physical Memory Manager (PMM)

Source: [`mem/pmm.c`](../../mem/pmm.c), [`mem/pmm.h`](../../mem/pmm.h)

The PMM manages physical RAM pages using a bitmap allocator and a `page_t` array. Memory regions are parsed from the Limine memory map at boot.

### Zones and page metadata

Physical memory is split into three zones:
- `PMM_ZONE_DMA`: 0 to 16 MB.
- `PMM_ZONE_DMA32`: 16 MB to 4 GB.
- `PMM_ZONE_NORMAL`: Above 4 GB.

Each 4KB page has a `page_t` struct in the global array:
- `flags`: Status bits (`PAGE_FLAG_FREE`, `PAGE_FLAG_RESERVED`, `PAGE_FLAG_KERNEL`, `PAGE_FLAG_USER`, `PAGE_FLAG_COW`, `PAGE_FLAG_ZERO`, `PAGE_FLAG_CACHED`, `PAGE_FLAG_DIRTY`, `PAGE_FLAG_LOCKED`, `PAGE_FLAG_SLAB`, `PAGE_FLAG_KMALLOC_LARGE`).
- `refcount`: Atomic page reference counter.
- `order`: Power-of-two allocation order (order 0 is 4KB, up to order 18).
- `zone`: Memory zone id.
- Freelist or page cache LRU links.

### Address conversions

Physical-to-virtual address translations use the direct map base offset from Limine:
- `pmm_paddr_to_page(paddr)` / `pmm_page_to_paddr(page)`
- `pmm_vaddr_to_page(vaddr)` / `pmm_page_to_vaddr(page)`

### API

- `pmm_alloc_page(flags)`: Allocates a single 4KB page.
- `pmm_alloc_order(order, flags)`: Allocates 2^order contiguous pages.
- `pmm_alloc_pages(count, flags)`: Allocates `count` contiguous pages.
- `pmm_free_page(page)` / `pmm_free_order(page, order)` / `pmm_free_pages(page, count)`: Frees pages.
- `pmm_page_ref(page)` / `pmm_page_unref(page)`: Updates reference count and frees the page when it hits 0.
- `pmm_get_stats()`: Returns total, free, reserved, kernel, and user page counts.

---

## MMU Driver

Source: [`mem/mmu.c`](../../mem/mmu.c), [`mem/mmu.h`](../../mem/mmu.h)

The MMU driver configures x86_64 4-level page tables (PML4, PDPT, PD, PT). Each address space uses an `mmu_context_t` struct holding the physical PML4 address and a spinlock.

### Flags

- `MMU_PROT_READ`: Read access.
- `MMU_PROT_WRITE`: Write access.
- `MMU_PROT_EXEC`: Execute access (NX bit clear).
- `MMU_PROT_USER`: User-mode access (Ring 3).
- `MMU_FLAG_NOCACHE`: Page-level cache disable (PCD).
- `MMU_FLAG_WC`: Write-combining cache mode.
- `MMU_FLAG_GLOBAL`: Global page bit.
- `MMU_FLAG_HUGE_2M`: 2MB page size.
- `MMU_FLAG_COW`: Custom PTE bit for copy-on-write tracking.

### API

- `mmu_create_context()` / `mmu_destroy_context(ctx)`: Allocates or destroys PML4 tables.
- `mmu_switch_context(ctx)`: Loads PML4 into `CR3`.
- `mmu_map_page(ctx, virt, phys, flags)`: Maps one page.
- `mmu_map_pages(ctx, virt, phys, count, flags)`: Maps multiple pages.
- `mmu_unmap_page(ctx, virt)` / `mmu_unmap_pages(ctx, virt, count)`: Unmaps pages and frees empty intermediate page tables.
- `mmu_protect_page(ctx, virt, flags)`: Modifies page protection bits.
- `mmu_virt_to_phys(ctx, virt)`: Resolves physical address from page tables.
- `mmu_clone_user_cow(parent_ctx, child_ctx)`: Clones userland mappings as read-only with `MMU_FLAG_COW` and increments page refcounts.

### TLB Shootdown

- `mmu_tlb_flush_page(virt)`: Local `invlpg`.
- `mmu_tlb_flush_all()`: Local `CR3` reload.
- `mmu_tlb_shootdown(virt, count)`: Local flush plus SMP IPI broadcast to invalidate remote TLBs.
- `mmu_tlb_shootdown_target(target_cpus, virt, count)`: Targeted SMP IPI TLB flush.

---

## Virtual Memory Areas (VMA)

Source: [`mem/vma.c`](../../mem/vma.c), [`mem/vma.h`](../../mem/vma.h), [`core/lib/rbtree.c`](../../core/lib/rbtree.c), [`core/lib/rbtree.h`](../../core/lib/rbtree.h)

Virtual memory ranges are represented by `vm_area_t`. They are stored in an augmented red-black tree (`rb_subtree_max_gap`) for finding unallocated address gaps, and in a sorted doubly linked list for traversal.

### Types and flags

- Types: `VMA_TYPE_ANON`, `VMA_TYPE_FILE`, `VMA_TYPE_DEVICE`, `VMA_TYPE_SHM`.
- Flags: `VMA_FLAG_READ`, `VMA_FLAG_WRITE`, `VMA_FLAG_EXEC`, `VMA_FLAG_SHARED`, `VMA_FLAG_COW`, `VMA_FLAG_STACK`, `VMA_FLAG_HEAP`.

### API

- `vma_create(start, end, type, flags)` / `vma_destroy(vma)`: Allocates or frees VMA structs.
- `vma_find(root, addr)`: Looks up VMA containing `addr`.
- `vma_find_unmapped_area(root, min_addr, max_addr, length, alignment)`: Finds a free virtual address range.
- `vma_insert(head, root, vma)` / `vma_remove(head, root, vma)`: Adds or removes VMA from list and RB-tree.
- `vma_merge_adjacent(head, root, prev, next)`: Merges adjacent compatible VMAs.
- `vma_split(vma, split_addr, out_left, out_right)`: Splits a VMA at `split_addr`.

---

## Virtual Memory Manager (VMM)

Source: [`mem/vmm.c`](../../mem/vmm.c), [`mem/vmm.h`](../../mem/vmm.h)

The VMM manages address spaces (`vmm_space_t`) and handles page faults.

### Address space (`vmm_space_t`)

- `mmu_ctx`: Associated MMU context.
- `vma_head`, `vma_tree`: VMA list and augmented RB-tree.
- `mmap_sem`: Reader-writer semaphore (`vmm_rwsem_t`).
- `active_cpus`: CPU bitmask executing on this address space.
- `mmap_base`, `mmap_end`: Dynamic allocation range.
- `brk_start`, `brk_current`: Program break positions.

### Page fault handling

`vmm_handle_page_fault()` processes page faults (Interrupt 14):
1. Finds the matching VMA for the fault address.
2. If invalid permissions or missing VMA, expands stack if near the stack VMA boundary, or faults.
3. For anonymous memory (`VMA_TYPE_ANON`):
   - Read faults map the zero page (`vmm_get_zero_page()`) read-only.
   - Write faults allocate a physical page, zero it, and map it read/write.
4. For copy-on-write faults:
   - If page refcount is 1, marks the page writable and clears `MMU_FLAG_COW`.
   - If page refcount > 1, allocates a new page, copies data, drops old refcount, and maps the new page read/write.
5. For file memory (`VMA_TYPE_FILE`):
   - Reads page from [Page Cache](pagecache.md) and maps it.

### Memory mapping API

- `vmm_map(space, hint, length, prot, flags, file, offset)`: Maps memory ranges (`mmap`).
- `vmm_unmap(space, addr, length)`: Unmaps memory ranges (`munmap`).
- `vmm_protect(space, addr, length, prot)`: Updates permissions (`mprotect`).
- `vmm_brk(space, new_brk)`: Resizes the heap (`brk`).
- `vmm_clone_space(parent_space)`: Clones address space for fork with COW.
- `vmalloc(size)` / `vfree(addr)`: Non-contiguous physical pages mapped contiguously in kernel virtual space.
