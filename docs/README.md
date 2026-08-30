<div align="center">
  <h1>BoredOS Documentation</h1>
  <p><em>Internal guides, architecture, and application development.</em></p>
</div>

---

Welcome to the documentation for BoredOS! This directory contains detailed guides on how the OS functions, how to build it, and how to develop applications for it.

## Table of Contents

The documentation is organized into three main categories:

### 1. [Architecture](architecture/)
Explains the logical layout of the kernel and internal components.

#### System
-   [`Core`](architecture/system/core.md): Kernel source layout and the boot process (Limine, Multiboot2).
-   [`Processes & Scheduling`](architecture/system/processes.md): Multitasking, context switching, and ELF loading.
-   [`TTY & Virtual Terminals`](architecture/system/tty.md): 10 virtual consoles, virtual framebuffers, active blitting, and keyboard/mouse multiplexing.
-   [`Interrupts & Exceptions`](architecture/system/interrupts.md): IDT, GDT, and exception handling.

#### Memory
-   [`Memory Architecture`](architecture/memory/memory.md): PMM frame allocator, MMU paging, VMA tracking, and demand paging in the VMM.
-   [`Slab Allocator`](architecture/memory/memory_manager.md): Object caches and general heap allocation (`kmalloc`/`kfree`).
-   [`Page Cache`](architecture/memory/pagecache.md): Page cache, radix tree indexing, and dirty page writeback.

#### Storage & Filesystems
-   [`Filesystem & VFS`](architecture/storage/filesystem.md): VFS layer, tmpfs, FAT32, ext4, and background sync.
-   [`AHCI Drivers`](architecture/storage/ahci_drivers.md): Hardware communication for block storage devices.

#### Network
-   [`Network Stack`](architecture/network/network_stack.md): TCP/IP implementation and socket APIs.
-   [`Network Drivers`](architecture/network/network_drivers.md): Hardware interaction for network cards (e.g. e1000).

#### Graphics
-   [`Framebuffer Device`](architecture/graphics/framebuffer.md): Direct physical screen memory access, Linux-compatible `/dev/fb0` ioctls, seeking, and memory mapping (`mmap`).

#### Hardware
-   [`PCI`](architecture/hardware/pci.md): PCI bus enumeration and device binding.
-   [`Input`](architecture/hardware/input.md): PS/2 Keyboard and Mouse input handling.

#### Misc
-   [`Versioning`](architecture/versioning.md): The OS date-based version scheme (`YY.M[.x]`) and kernel semantic versioning (`MAJOR.MINOR.PATCH`).

### 2. [Building and Deployment](build/)
Instructions for compiling the OS from source.
-   [`Toolchain`](build/toolchain.md): Prerequisites and cross-compiler setup (`x86_64-elf-gcc`, `nasm`, `xorriso`).
-   [`Usage`](build/usage.md): Understanding the Makefile targets, QEMU emulation, and flashing to bare metal hardware.

### 3. [Application Development](appdev/)
The SDK and toolchain guides for creating your own `.elf` userland binaries.
-   [`SDK Reference`](appdev/sdk_reference.md): Overview hub for SDK layout, includes, and links to detailed libc/syscall docs.
-   [`Syscalls`](appdev/syscalls.md): Current syscall numbers, FS/SYSTEM command IDs, and wrapper guidance.
-   [`libc Reference`](appdev/libc_reference.md): Current libc headers, implemented APIs, and behavior notes.
-   [`Raw Graphics Guide`](appdev/framebuffer_drawing.md): Developer guide to raw `/dev/fb0` drawing, screen layout queries, ioctls, memory mapping (`mmap`), and safe TTY mode restoration.
-   [`Custom Apps`](appdev/custom_apps.md): A step-by-step tutorial on writing a new C application, editing the Makefile, and compiling/bundling it into the ISO.
-   [`Example Apps`](appdev/examples/README.md): A collection of sample C applications ranging from basic terminal output to advanced direct framebuffer access and TCP networking.
-   [`Native TCC`](appdev/tcc.md): How to use the Tiny C Compiler (TCC) to build and run C applications directly on BoredOS.
-   [`Nova Protocol`](appdev/nova/nova_protocol.md): Detailed wire protocol formats and event layouts for the desktop compositor.
-   [`Developing Nova Clients`](appdev/nova/nova_development.md): Guide for building applications using the low-level Nova compositor socket interface.
-   [`Nova ToolKit (NTK)`](appdev/nova/ntk.md): Official widget toolkit for developing graphical applications with decorations and layouts.

### 4. [Usage](usage/)
General guides on how to interact with the OS.
-   [`Booting`](usage/booting.md): How to use the Limine bootloader and toggle kernel boot flags like `-v`.
-   [`Terminal`](usage/terminal.md): Command line interface, redirection, and common commands.

---
