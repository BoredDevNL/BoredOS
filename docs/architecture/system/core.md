# Core Architecture

Overview of BoredOS kernel layout, boot sequence, and subsystem initialization.

## Source Code Layout

The kernel source tree is organized into the following directories:

- **`arch/`**: Low-level assembly routines for bootstrap, GDT/IDT management, FPU/SSE state, interrupts, and syscall entry stubs.
- **`core/`**: Initialization entry point ([`main.c`](../../core/main.c)), kernel utilities ([`kutils.c`](../../core/kutils.c)), data structures ([`rbtree.c`](../../core/lib/rbtree.c)), and panic handling.
- **`dev/`**: Device drivers including PCI scanning, AHCI SATA controller, PS/2 input, AC97 audio, and RTC.
- **`fs/`**: Virtual File System ([`vfs.c`](../../fs/vfs.c)), [tmpfs](../../fs/tmpfs.c), [FAT32](../../fs/fat32.c), [ext4](../../fs/ext4fs.c), ProcFS, SysFS, and writeback flusher ([`flusher.c`](../../fs/flusher.c)).
- **`mem/`**: Memory subsystem including [PMM](../../mem/pmm.c), [MMU](../../mem/mmu.c), [VMA](../../mem/vma.c), [VMM](../../mem/vmm.c), [Slab allocator](../../mem/slab.c), [Page cache](../../mem/pagecache.c), and [Radix tree](../../mem/radix_tree.c).
- **`net/`**: Networking stack using lwIP with NIC drivers ([`net/nic/`](../../net/nic/)) and socket buffer layer.
- **`sys/`**: Process scheduler ([`process.c`](../../sys/process.c)), system call dispatcher ([`syscall.c`](../../sys/syscall.c)), SMP management ([`smp.c`](../../sys/smp.c)), futexes, and ELF loader.
- **`graphics/`**: Graphical primitives, console font rendering, and framebuffer management.
- **`usr/`**: Userspace libraries (such as mlibc) and applications (Nova compositor, core utilities, packages).

---

## Boot Sequence

BoredOS boots via **Limine**:

1. **Bootloader**: Limine loads the kernel ELF into higher-half memory, sets up initial page tables, passes framebuffer info, and responds to boot requests (HHDM, SMP, memmap).
2. **Kernel Entry (`_start` in `core/main.c`)**:
   - Initializes serial logging and GDT/IDT.
   - Initializes memory: `pmm_init()` -> `slab_init()` -> `mmu_init()` -> `vmm_init()` -> `pagecache_init()`.
   - Initializes BSP SMP state via `smp_init_bsp()`.
   - Initializes ACPI and timer interrupts.
   - Initializes process management (`process_init()`) and futexes (`futex_init()`).
   - Initializes storage drivers: FAT32, disk manager, AHCI scan.
   - Initializes VFS mounts: `/sys` (sysfs), `/proc` (procfs), `/` (tmpfs), and switches root to disk if specified.
   - Starts the background writeback flusher thread (`flusher_init()`).
3. **AP Bringup**: `smp_init()` boots Application Processors using SIPI sequences. Each core initializes its local GDT, TSS, and idle scheduler loop.
4. **Userspace Startup**: The kernel loads the init process or shell binary from the filesystem into a userland address space and switches to Ring 3.

---

## Multiprocessing & Scheduling

- **SMP**: Multi-core scheduling uses Local APIC timer interrupts and IPIs (vector `0x41`) to trigger scheduling passes across cores.
- **Scheduler**: Round-robin scheduler running on each core. Processes have CPU affinity masks and sleep/wake wait queues.
- **Synchronization**: Kernel data structures use interrupt-safe spinlocks and reader-writer semaphores.

---

## Userspace Execution

When launching a userspace program:
1. Allocates an address space (`vmm_space_t`) with its own PML4 table.
2. Maps ELF segments into VMAs and demand-pages memory.
3. Sets up user stack and copies argument strings (`argv`, `envp`).
4. Switches to Ring 3 via `iretq` or `sysret`.
5. Userspace uses the `syscall` instruction to invoke kernel system calls.
