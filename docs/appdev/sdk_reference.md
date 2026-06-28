<div align="center">
  <h1>Userland SDK Reference</h1>
    <p><em>Overview and entry point for BoredOS userland development.</em></p>
</div>

---

BoredOS provides a compact userland SDK for building `.elf` applications.
This page is the high-level map; detailed API references now live in dedicated pages.

## SDK Structure

Primary headers are in `contrib/libc/include/`.

- `stdlib.h`, `string.h`, `stdio.h`, `unistd.h`: core libc surface
- `syscall.h`: raw syscall wrappers and command constants
- `math.h`: freestanding math helpers
- `sys/ioctl.h`: device-specific I/O control operations (like framebuffer parameters, TTY window sizes, and TTY mode settings)
- `sys/kd.h`: console graphics mode definitions (`KDSETMODE`, `KD_GRAPHICS`, `KD_TEXT`)

## Detailed References

- [`libc Reference`](libc_reference.md): current libc headers and implemented APIs
- [`Syscalls`](syscalls.md): syscall numbers, FS/SYSTEM command IDs, and wrappers
- [`Raw Graphics Guide`](framebuffer_drawing.md): raw framebuffer (`/dev/fb0`) drawing, ioctls, mmap, and TTY recovery
- [`Native TCC`](tcc.md): Native C compilation directly on BoredOS
- [`Nova Protocol`](nova/nova_protocol.md): Compositor wire format protocols and event messaging.
- [`Nova Toolkit (NTK)`](nova/ntk.md): Native C object-based widget library for graphical GUI software.

## Typical Include Set

```c
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <syscall.h>
#include <sys/ioctl.h>
```

For Direct Framebuffer (Graphics) apps:

```c
#include <sys/ioctl.h>
#include <sys/kd.h>
```

For raw Nova compositor clients:

```c
#include <novaproto.h>
```

For NTK GUI applications:

```c
#include <ntk.h>
```

## Build and Packaging

- Add app source under the appropriate external repository source directory (for example, `contrib/coreutils/src/`, `contrib/nova/src/`, or a new `contrib/<repo>/src/`).
- Ensure it is included in the userland build rules/targets.
- Build from repo root with `make`.
- Built binaries are copied into initrd under `/bin` by the top-level `Makefile`.


