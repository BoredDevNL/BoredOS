<div align="center">
  <h1>Nova Overview</h1>
  <p><em>What Nova is, how it fits into BoredOS, and the high-level surface model.</em></p>
</div>

---

Nova is the userland compositor and window manager for BoredOS. It runs as a dedicated process and provides a graphical display server that connects client applications to the physical framebuffer (`/dev/fb0`) and input devices.

## What Nova does

Nova is responsible for:

- Accepting client connections over a UNIX domain socket (`/tmp/nova.sock`).
- Exposing a compact wire protocol for surface creation, damage reporting, movement, resizing, title/icon updates, and window state.
- Allocating shared memory segments for each surface and handing the shared memory path back to client apps.
- Compositing surface pixels into a double-buffered backbuffer and presenting to the framebuffer.
- Routing keyboard and mouse events to the focused application surface.
- Managing window focus, decorations, resizing, minimize/close behavior, and overlay clients like taskbars.
- Autostarting UI services such as shelf, taskbar, and wallpaper from `/etc/nova/nova.conf`.

## Where Nova lives in source

- `contrib/nova/src/nova.c` — the compositor process and the runtime manager.
- `contrib/nova/libnovaproto/novaproto.h` — wire protocol definitions and client API declarations.
- `contrib/nova/libnovaproto/novaproto.c` — client-side protocol implementation and event queue.
- `contrib/nova/src/helloworld.c`, `contrib/nova/src/taskbar.c`, `contrib/nova/src/wallpaperd.c` — example Nova clients.

## Core architecture

Nova is built around two main pieces:

1. **The Nova compositor**
   - Opens `/dev/fb0` and switches the terminal console into graphics mode.
   - Creates a UNIX socket server at `/tmp/nova.sock`.
   - Maintains an ordered surface list and renders surfaces in z-order.
   - Uses a backbuffer for safe composition and only flushes damaged regions to the physical framebuffer.

2. **The Nova protocol and client library**
   - Clients connect using `nova_connect()`.
   - The compositor allocates a surface and returns a shared memory pathname.
   - Clients write pixel data directly into the mapped shared memory and notify Nova with `nova_damage_surface()`.
   - Nova sends event messages back to clients: keyboard, pointer, resize requests, close requests, focus changes, and window list updates.

## Surface model

A Nova surface is the fundamental rendering unit. Each surface is:

- Identified by a `surface_id` assigned by the compositor.
- Backed by a shared-memory pixel buffer exposed via a pathname such as `/dev/shm/nova_surf_fd%d_%u`.
- Assigned a `layer` value that controls z-order and decoration rules.
- Optionally decorated with a titlebar, window border, close/minimize controls, and drop shadow.
- Marked as `mapped` when visible; minimized/hidden surfaces are simply unmapped.
- Marked as `focused` when the compositor has returned input focus to the surface.

## Layer conventions

The compositor recognizes multiple visual layers. Common values observed in the source are:

- `0` — desktop/background surfaces.
- `1` — normal application windows with decorations.
- `2` — floating windows with decorations.
- `3` — overlay surfaces (e.g. taskbar, shelf, dock) without title decorations.
- `4` — modal or popup menus shown above application windows.
- `5` — additional top-layer overlays.

Nova renders layers from `0` up to `5` in ascending order.

> Note: The Nova protocol header does not expose explicit layer name constants, so clients define their own layer constants when needed.

## Input, focus, and event routing

Nova captures raw keyboard and mouse events directly from `/dev/keyboard` and `/dev/mouse`.

- Keyboard input is translated from PS/2 scancodes to `NovaKeycode` values.
- Mouse interactions perform hit-testing against surface bounds and titlebars.
- A focused surface receives events such as `EVT_KEY` and `EVT_POINTER`.
- Clicking a window titlebar can start move or resize drags.
- Nova sends `EVT_RESIZE_REQUEST` to clients when the compositor needs them to resize their surface contents.

## Themes and autostart

Nova loads UI theme settings from `/etc/nova/nova.conf`.

- Color values such as `active_titlebar_top` and `inactive_border` are configurable.
- Autostart entries like `wallpaperd` are also read from the configuration file.
- The compositor responds to `SIGUSR1` by reloading the config file at runtime.

## Recommended reading

- [`Nova Protocol Reference`](nova_protocol.md)
- [`Developing Nova Clients`](nova_development.md)
- [`Nova Compositor Internals`](nova_compositor.md)
