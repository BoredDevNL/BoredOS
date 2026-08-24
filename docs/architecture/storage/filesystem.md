# Filesystem & Storage Architecture

BoredOS implements a Virtual Filesystem (VFS) layer to support filesystems, virtual filesystems, and device nodes.

## 1. Virtual File System (VFS)

Source: [`fs/vfs.c`](../../fs/vfs.c), [`fs/vfs.h`](../../fs/vfs.h)

The VFS dispatches POSIX file operations to mounted filesystem drivers via `vfs_fs_ops_t`.

Key operations in `vfs_fs_ops_t`:
- File operations: `open`, `close`, `read`, `write`, `seek`, `poll`, `ioctl`.
- Directory operations: `readdir`, `mkdir`, `rmdir`, `unlink`, `rename`.
- Metadata and sync: `exists`, `is_dir`, `get_info`, `statfs`, `sync_fs`, `unmount`, `writepage`.

### Mount management

- `vfs_mount(mount_path, device, fs_type, ops, fs_private)`: Registers a mount point in the mount table (up to 64 active mounts).
- `vfs_umount(mount_path)`: Unmounts a filesystem.
- `vfs_normalize_path(cwd, path, normalized)`: Resolves relative paths, `.`, and `..`.

---

## 2. Filesystems

### tmpfs

Source: [`fs/tmpfs.c`](../../fs/tmpfs.c), [`fs/tmpfs.h`](../../fs/tmpfs.h)

`tmpfs` is a RAM-backed filesystem where file contents are stored in [Page Cache](../memory/pagecache.md) pages (`address_space_t`).
- Used as the default root filesystem (`/`) during early boot.
- Mounted at `/tmp` for temporary files.

### FAT32

Source: [`fs/fat32.c`](../../fs/fat32.c), [`fs/fat32.h`](../../fs/fat32.h)

FAT32 driver supporting read, write, directory traversal, cluster chain allocation, and volume mounting over disk partitions. Auto-mounted at `/boot` for ESP partitions.

### ext4

Source: [`fs/ext4fs.c`](../../fs/ext4fs.c)

ext4 filesystem driver supporting volume mounting, file reading, and directory traversal on disk partitions.

### Virtual Filesystems

- `procfs` (`/proc`): Exposes process information and system state.
- `sysfs` (`/sys`): Exposes hardware device trees and bus topology.
- `shm` (`/dev/shm`): Shared memory file provider for IPC.

---

## 3. Storage and Disk Manager

Source: [`dev/disk_manager.c`](../../dev/disk_manager.c), [`dev/ahci.c`](../../dev/ahci.c)

- **AHCI**: Probes SATA controllers and performs disk I/O via DMA transfers.
- **Disk Manager**: Scans disks, parses MBR and GPT partition tables, and creates block device entries under `/dev` (such as `/dev/sda1`).
- **Writeback Flusher**: [`fs/flusher.c`](../../fs/flusher.c) runs a background thread that periodically flushes dirty page cache pages to disk using `vfs_sync_all()`.
