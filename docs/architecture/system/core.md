<div align="center">
  <h1>Core Architecture</h1>
  <p><em>Overview of BoredOS kernel layout, boot process, and userspace transition.</em></p>
</div>

---

BoredOS is a 64-bit hobbyist operating system designed for the x86_64 architecture. While it features kernel-space drivers and advanced console TTYs, it supports fully-isolated userspace applications and includes a networking stack.

This document serves as an overview of the core architecture and the layout of the kernel source code.

##  Source Code Layout (`src/`)

The OS heavily relies on module separation. The `src/` directory is logically split into several domains:

- **`arch/`**: Contains the assembly routines needed for bootstrapping the system (`boot.asm`) and setting up the CPU state for userland execution (`process_asm.asm`). It also handles architecture-specific mechanisms like the Global Descriptor Table (GDT) and Interrupt Descriptor Table (IDT).
- **`core/`**: The initialization sequence of the OS lives here. `main.c` is the entry point from the bootloader. This directory also contains essential kernel utilities (`kutils.c`), panic handlers (`panic.c`), and built-in command parsing logic (`cmd.c`).
- **`dev/`**: Device drivers. This includes the PCI scanner, disk management infrastructure, input drivers (keyboard and mouse), and the Real Time Clock (RTC).
- **`fs/`**: Filesystem implementations. The system uses a Virtual File System (VFS) abstraction alongside an in-memory FAT32 filesystem with support for drives over ATA that are formatted as FAT32 (plain/MBR).
- **`mem/`**: Physical and virtual memory management. It controls page frame allocation, paging, and kernel heap operations.
- **`net/`**: The networking stack. BoredOS relies on `lwIP` for processing IPv4 and TCP/UDP traffic, interacting with a range of NICs via `net/nic/`.
- **`sys/`**: System calls and process management. The ELF loader resides here, alongside the Symmetric Multi-Processing (**smp.c**) bringup and Local APIC (**lapic.c**) management logic.
- **`graphics/`**: The graphical subsystem. It handles drawing primitives, graphics contexts, font rendering, and double-buffering.
- **`contrib/`**: Out-of-kernel components are organized into separate external repositories. This includes the custom SDK/compiler environment (`contrib/libc/`) and user applications such as `contrib/coreutils/`, `contrib/nova/`, and other external userland repositories.

## Boot Process

BoredOS uses **Limine** as its primary bootloader.

1.  **Limine Initialization**: The machine firmware (BIOS or UEFI) loads Limine. Limine parses `limine.conf`, sets up an early graphical framebuffer, and reads the kernel ELF file into memory.
2.  **Multiboot2 & SMP Protocol**: The kernel expects the Limine boot protocol. It makes a specific **SMP Request** to Limine to locate and bring up all available CPU cores.
3.  **Kernel Entry (`main.c`)**: The entry point `_start` is called on the Bootstrap Processor (BSP). It initializes the serial port, GDT/IDT, memory management, and paging.
4.  **AP Bringup**: The BSP calls `smp_init()`, which sends the Startup Inter-Processor Interrupt (SIPI) sequence to all Application Processors (APs). Each AP initializes its own local GDT, TSS, and Page Tables before entering an idle loop.
5.  **Driver Initialization**: PCI buses are scanned, finding the network card or disk controllers. The filesystem is mounted.
6.  **TTY and Console**: The system initializes 10 virtual consoles (TTYs) and launches the standard command-line shell (`/bin/bsh.elf`) on the active TTY.

## Multi-Core & Scheduling

BoredOS utilizes Symmetric Multi-Processing (SMP) to distribute workloads across all available CPU cores.

-   **LAPIC & IPIs**: Each CPU has its own Local APIC. The kernel uses Inter-Processor Interrupts (IPIs) for inter-core communication, specifically for triggering the scheduler on other cores (`vector 0x41`).
-   **Scheduler**: A round-robin scheduler runs on each core. Processes are pinned to specific CPUs (CPU Affinity) to maintain cache locality. The BSP timer interrupt (`60Hz`) broadcasts a scheduling IPI to all core to ensure balanced execution.
-   **Spinlocks**: Since multiple cores can access kernel structures (VFS, Process List) simultaneously, the kernel uses **interrupt-safe spinlocks** to prevent race conditions.

## Userland Transition

The OS supports privilege separation (Ring 0 vs. Ring 3). When an application is launched, the kernel:

1.  Loads the ELF file from the filesystem.
2.  Assigns the process to a CPU core via a round-robin distribution strategy.
3.  Allocates a new virtual address space (Page Directory) for the process.
4.  Maps the executable segments according to the ELF headers.
5.  Switches to User Mode (Ring 3) via the `iretq` instruction.

> [!IMPORTANT]
> Programs interact with the core kernel using system calls (`syscall.c`). Multitasking is achieved by pre-empting user processes on their respective cores.

---
