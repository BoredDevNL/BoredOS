# BoredOS Architecture

This folder gathers the architecture documentation that explains how BoredOS is built from the kernel up.

## Architecture roadmap

The documentation is split by area so you can go directly to the subsystem you want to understand.

| Area | Document | Description |
| --- | --- | --- |
| Graphics | [`graphics/framebuffer.md`](graphics/framebuffer.md) | Framebuffer device (/dev/fb0) architecture, ioctls, seeking, and direct screen memory. |
| Hardware | [`hardware/input.md`](hardware/input.md) | Hardware-level input support and device wiring. |
| Hardware | [`hardware/pci.md`](hardware/pci.md) | PCI bus management and device enumeration. |
| Hardware | [`hardware/i2c.md`](hardware/i2c.md) | I2C bus management and controller drivers. |
| System | [`ACPI/acpi_interface.md`](ACPI/acpi_interface.md) | Power management and ACPI hardware discovery. |
| Input | [`input/keyboard.md`](input/keyboard.md) | Keyboard input handling and key mapping. |
| Memory | [`memory/memory.md`](memory/memory.md) | Physical memory (PMM), 4-level MMU, VMA tree, and VMM. |
| Memory | [`memory/memory_manager.md`](memory/memory_manager.md) | Slab allocator and kernel heap (`kmalloc`/`kfree`). |
| Memory | [`memory/pagecache.md`](memory/pagecache.md) | Page cache, radix tree indexing, and dirty writeback. |
| Network | [`network/network_stack.md`](network/network_stack.md) | TCP/IP stack design, protocol flow, and packet handling. |
| Network | [`network/network_drivers.md`](network/network_drivers.md) | Network driver architecture and interface support. |
| Storage | [`storage/filesystem.md`](storage/filesystem.md) | VFS, tmpfs, FAT32, ext4, and background sync. |
| Storage | [`storage/ahci_drivers.md`](storage/ahci_drivers.md) | AHCI driver implementation and disk controller support. |
| System | [`system/core.md`](system/core.md) | Core kernel architecture and main subsystems. |
| System | [`system/tty.md`](system/tty.md) | Virtual terminals, virtual framebuffers, active TTY blitting, and keyboard/mouse multiplexing. |
| System | [`system/interrupts.md`](system/interrupts.md) | Interrupt handling and low-level event dispatch. |
| System | [`system/processes.md`](system/processes.md) | Process management, scheduling, and execution model. |
| General | [`versioning.md`](versioning.md) | Release versioning and project numbering conventions. |

## Quick start

- **Read `system/core.md` first** for the kernel overview.
- Then explore the subsystem area you need: `memory/`, `network/`, `storage/`, `graphics/`, or `system/`.
- Use `versioning.md` to understand BoredOS version rules.

> Note: The links above point directly to the most important architecture documents in this folder.
