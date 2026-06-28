<div align="center">
  <h1>Developing Nova Clients</h1>
  <p><em>How to build GUI applications that speak the Nova protocol.</em></p>
</div>

---

BoredOS supports two levels of graphical client development:
1. **High-Level (Recommended):** The [Nova Toolkit (NTK)](ntk.md) is a feature-rich GUI toolkit that manages window frames, styling, layouts, widgets, input redirection, and event dispatching.
2. **Low-Level (Direct Protocol):** The raw `libnovaproto` API, which communicates directly with the Nova socket `/tmp/nova.sock` using custom shared memory segments, dirty damage lists, and low-level wire frames.

This guide explains the low-level, direct protocol approach for developers who need manual control or want to implement custom rendering/toolkits. For standard application development, please refer to the [NTK Developer Guide](ntk.md).

## 1. Include the Nova API

Your Nova client should include the protocol header and any UI helper libraries you need.

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include "libnovaproto/novaproto.h"
```

If your build environment exposes the SDK include path, you can also use `#include <novaproto.h>`.

## 2. Connect to the compositor

Nova listens on `/tmp/nova.sock` by default. The client library also supports the `NOVA_SOCKET` environment variable.

```c
int fd = nova_connect(NULL);
if (fd < 0) {
    fprintf(stderr, "Cannot connect to Nova socket\n");
    return 1;
}
```

## 3. Create a surface and map its shared buffer

Create a surface using `nova_create_surface()` and map the returned shared memory pathname.

```c
uint32_t surf_id;
char shm_path[128];
uint32_t width = 300, height = 200;
if (nova_create_surface(fd, width, height, 1, 0, &surf_id, shm_path) < 0) {
    perror("nova_create_surface");
    return 1;
}

int shm_fd = open(shm_path, O_RDWR);
if (shm_fd < 0) {
    perror("open shm_path");
    return 1;
}

uint32_t *pixels = mmap(NULL, width * height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
close(shm_fd);
if (pixels == MAP_FAILED) {
    perror("mmap");
    return 1;
}
```

## 4. Draw into the surface

Write pixels directly into the shared buffer. When the image changes, tell Nova which regions are dirty.

```c
NovaRect damage = { 0, 0, width, height };
nova_damage_surface(fd, surf_id, 1, &damage);
```

Only damaged regions need to be reported, but full-screen damage is the simplest starting point.

## 5. Handle Nova events

Nova clients should poll both the compositor socket and any internal event queue.

```c
struct pollfd pfd;
pfd.fd = fd;
pfd.events = POLLIN;

while (running) {
    int timeout = 100;
    int pr = poll(&pfd, 1, timeout);

    if (pr < 0) break;

    if ((pr > 0 && (pfd.revents & POLLIN)) || nova_pending_events()) {
        NovaEvent ev;
        while (nova_poll_event(fd, &ev) == 0) {
            switch (ev.type) {
                case EVT_CLOSE_REQUEST:
                    running = false;
                    break;
                case EVT_RESIZE_REQUEST:
                    handle_resize(ev.data.resize.w, ev.data.resize.h);
                    break;
                case EVT_KEY:
                    handle_key(ev.data.key.keycode, ev.data.key.modifiers, ev.data.key.pressed);
                    break;
                case EVT_POINTER:
                    handle_pointer(ev.data.pointer.x, ev.data.pointer.y, ev.data.pointer.buttons);
                    break;
                case EVT_FOCUS_IN:
                case EVT_FOCUS_OUT:
                    update_focus_state(ev.type == EVT_FOCUS_IN);
                    break;
            }
        }
    }
}
```

### Resize handling

The compositor will request a new size with `EVT_RESIZE_REQUEST`.

```c
void handle_resize(uint32_t new_w, uint32_t new_h) {
    char new_shm_path[128];
    if (nova_resize_surface(fd, surf_id, new_w, new_h, new_shm_path) < 0) return;

    int new_shm_fd = open(new_shm_path, O_RDWR);
    if (new_shm_fd < 0) return;

    uint32_t *new_pixels = mmap(NULL, new_w * new_h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, new_shm_fd, 0);
    close(new_shm_fd);
    if (new_pixels == MAP_FAILED) return;

    munmap(pixels, width * height * 4);
    pixels = new_pixels;
    width = new_w;
    height = new_h;
    redraw();
}
```

## 6. Update title, icon, and state

Use these helpers to keep the compositor informed about window metadata.

```c
nova_set_title(fd, surf_id, "My Nova App");
nova_set_icon(fd, surf_id, "/path/to/icon.png");

// Request focus or update active state flags.
nova_set_state(fd, surf_id, 1);
```

## 7. Destroy the surface and clean up

Before exiting, destroy the surface and release mapped memory.

```c
nova_destroy_surface(fd, surf_id);
munmap(pixels, width * height * 4);
close(fd);
```

## Best practices

- Always call `nova_damage_surface()` after changing pixels.
- Handle `EVT_RESIZE_REQUEST` by re-mapping the new shared memory path.
- Use `nova_set_title()` and `nova_set_icon()` to keep the window list and taskbar in sync.
- Use `nova_pending_events()` to drain internal events without blocking.
- Keep your event loop responsive so the compositor can handle input smoothly.

## Real-world examples

Look at these example apps for practical Nova usage:

- `contrib/nova/src/helloworld.c`
- `contrib/nova/src/taskbar.c`
- `contrib/nova/src/wallpaperd.c`

## Notes

- Nova's current header does not expose named layer constants. Define your own, e.g. `#define NORMAL_LAYER 1`.
- The shared memory segment will be created by Nova and returned in `shm_path`.
- `MSG_QUIT` is available, but requesting the compositor to exit is typically reserved for shell or system-level tools.
