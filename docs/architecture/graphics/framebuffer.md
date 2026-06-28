# Framebuffer Device (/dev/fb0) Architecture

In BoredOS, graphical output is managed via a standard, Linux-compatible framebuffer device node registered at `/dev/fb0`. This node provides a raw interface for userland applications to access and manipulate physical screen pixels directly.

---

## 1. VFS Registration and Routing

The virtual file system (VFS) in [`src/fs/vfs.c`](../../../src/fs/vfs.c) manages all files under the `/dev` hierarchy. When an application opens `/dev/fb0`, the kernel assigns it a special device type:

```c
#define DEVICE_TYPE_FRAMEBUFFER 2
```

Once opened, standard file descriptor operations (`read`, `write`, `lseek`, `ioctl`, `mmap`) on this descriptor are intercepted by the VFS and routed to dedicated framebuffer device operations rather than standard disk/filesystem operations.

---

## 2. Direct Physical Screen Operations

Unlike a standard file, `/dev/fb0` maps directly to the active physical screen memory allocated during the boot phase (configured by Limine). 

At boot, the kernel queries the physical screen configuration using:

```c
graphics_fb_params_t params = graphics_get_fb_params();
```

This returns the physical hardware framebuffer parameters:
- **`address`**: The physical start address of display memory (e.g. `0xfd000000`).
- **`width`**: Display width in pixels (e.g. `1024`).
- **`height`**: Display height in pixels (e.g. `768`).
- **`bpp`**: Bits per pixel (always `32` for BoredOS, packing BGRA).
- **`pitch`**: The actual physical line length in bytes (width * 4 + alignment padding).

### Write and Seek Operations
When a userland application writes to `/dev/fb0`:
1. The kernel maintains a seek offset `pos` inside the VFS file descriptor structure.
2. The userland writes a byte array of pixel data.
3. The VFS checks that the write is within the bounds of screen memory (`width * height * 4` bytes).
4. The kernel performs a direct `memcpy` from the user's buffer to the physical address offset:
   ```c
   memcpy((void*)(g_fb->address + pos), user_buf, size);
   ```
5. The descriptor offset `pos` is advanced by the number of bytes successfully written.
6. The `lseek()` system call is fully supported, allowing applications to reset the pixel cursor back to `0` (start of screen) or seek to a precise pixel offset before writing.

---

## 3. Supported Linux-Compatible Ioctls

To support standard porting of unix graphic utilities (such as terminal emulators and future display servers), `/dev/fb0` implements standard Linux `ioctl` commands. These structures are defined in [`contrib/libc/include/sys/ioctl.h`](../../../contrib/libc/include/sys/ioctl.h):

### `FBIOGET_VSCREENINFO` (0x4600)
Returns variable screen information using the `fb_var_screeninfo_t` struct:

| Field | Value | Meaning |
|---|---|---|
| `xres` / `yres` | e.g. `1024` / `768` | Active visible resolution |
| `xres_virtual` / `yres_virtual` | e.g. `1024` / `768` | Virtual resolution |
| `bits_per_pixel` | `32` | Color depth (4 bytes per pixel) |
| `red` / `green` / `blue` | 8-bit sizes, specific offsets | Tells userland the exact bit configuration of colors (BGRA layout) |

### `FBIOGET_FSCREENINFO` (0x4602)
Returns fixed hardware parameters using the `fb_fix_screeninfo_t` struct:

| Field | Value | Meaning |
|---|---|---|
| `smem_start` | e.g. `0xfd000000` | Physical start address of display memory |
| `smem_len` | `line_length * yres` | Total size of display memory |
| `visual` | `FB_VISUAL_TRUECOLOR` (2) | Layout type (TrueColor packed BGRA) |
| `line_length` | e.g. `4096` | Pitch of a single line in bytes |

---

## 4. Zero-Copy Performance via `mmap`

BoredOS supports memory mapping for high-performance, zero-copy graphics rendering. 

When a process invokes the `sys_mmap` system call on `/dev/fb0`'s file descriptor:
1. The kernel validates that the offset is valid and that the requested size fits within the physical framebuffer limits (`smem_len`).
2. The virtual memory manager (VMM) maps the physical address range (`g_fb->address`) directly into the process's page table at an allocated user virtual address.
3. This mapping is created with **Read/Write permissions** and caching policies suitable for graphics memory (Write-Combining).
4. The userland process receives a direct pointer to screen memory:
   ```c
   uint32_t *pixels = (uint32_t *)mmap(NULL, screen_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
   ```
5. Writes to this pointer immediately modify pixels on the physical screen without any system call context-switch overhead, achieving maximum possible rendering performance.

---

## 5. Console Coexistence and Mode Toggling

Since the kernel contains an active text console blitting loop, direct writes to `/dev/fb0` would normally be clobbered by terminal text characters whenever a background tick fires.

To prevent this, userland applications must open their active controlling TTY console (`/dev/tty0` or standard input descriptor `0`) and execute:

```c
ioctl(0, KDSETMODE, (void*)KD_GRAPHICS);
```

This sets the TTY blit toggle to `false`, pausing the background console text rendering. The application has exclusive, uninterrupted control of the display. 

Before exiting, the application MUST restore text mode to resume normal terminal behavior:

```c
ioctl(0, KDSETMODE, (void*)KD_TEXT);
```

---
