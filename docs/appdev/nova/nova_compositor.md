<div align="center">
  <h1>Nova Compositor Internals</h1>
  <p><em>How the Nova compositor manages surfaces, input, shared memory, and framebuffer composition.</em></p>
</div>

---

Nova's compositor implementation lives in `contrib/nova/src/nova.c`. It is a self-contained userland process that manages input, clients, shared memory, and direct framebuffer rendering.

## Main runtime flow

1. Load theme and autostart settings from `/etc/nova/nova.conf`.
2. Open `/dev/fb0` and query screen dimensions with `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`.
3. Allocate a software backbuffer and an optional resize preview buffer.
4. Switch the console into graphics mode using `KD_GRAPHICS`.
5. Map the framebuffer into memory with `mmap()`.
6. Initialize the socket server at `/tmp/nova.sock`.
7. Open keyboard and mouse devices in non-blocking mode.
8. Spawn autostart services such as taskbar and wallpaper.
9. Enter the main poll loop.

## Client connection management

Nova maintains an array of active clients and a linked list of surfaces.

- New clients connect via `accept()` on the UNIX domain socket.
- Each client can create multiple surfaces.
- The compositor stores `client_fd` on each surface for routing events.
- When a socket closes, Nova tears down all surfaces owned by that client.

## Surface state

Each surface is represented by a `surface_t` structure containing:

- `surface_id`
- `client_fd`
- `x, y, w, h`
- `layer`
- `flags`
- `title` and `icon_path`
- `state_flags`
- `shm_path`, `pixels`, and `shm_size`
- Focus and dragging/resizing state
- Pending resize metadata for smooth transitions

### Mapped vs unmapped

- `mapped = true` means the surface is visible and composited.
- Minimizing or hiding a window sets `mapped = false`.
- Unmapped surfaces remain in the surface list until destroyed.

## Surface rendering and layers

Nova renders surfaces in layer order from `0` through `5`.

- Layers `1` and `2` receive window decorations and focus handling.
- Layer `3` is used for overlay clients such as taskbars and dock panels.
- Other layers are treated as content-only and may be used for popups or backgrounds.

The compositor uses a dirty rectangle system to redraw only the areas that changed.

## Shared memory workflow

When a client requests a surface:

- Nova allocates a unique shared memory pathname like `/dev/shm/nova_surf_fd%d_%u`.
- It writes zeroes into the new shm file to reserve the full size.
- It then maps the bytes into its own address space and exposes the path to the client.
- Clients `open()` and `mmap()` that file and draw directly into shared memory.

For resizing:

- Nova allocates a new `_v2` shared memory segment.
- It returns the new path to the client via `MSG_RESIZE_SURFACE`.
- Upon the next `MSG_DAMAGE`, Nova commits the pending resized buffer and replaces the old mapping.

## Input handling

Nova polls the keyboard and mouse devices directly and translates raw PS/2 scancodes into `NovaKeycode` values.

- Modifier state for Shift, Ctrl, and Alt is tracked internally.
- Keyboard events are delivered to the focused surface via `EVT_KEY`.
- Mouse hit testing uses `surface_at()` and determines click regions such as titlebar, close button, minimize button, and resize edges.
- Pointer events are delivered to surface content regions as `EVT_POINTER`.
- Clicking the titlebar begins move dragging; clicking window borders begins resize dragging.

## Window management operations

Nova implements several window manager behaviors:

- `set_focus()` updates the active surface and sends `EVT_FOCUS_IN` / `EVT_FOCUS_OUT`.
- `toggle_maximize()` resizes a surface to fill the screen while preserving its normal geometry.
- `broadcast_window_event()` sends `EVT_WINDOW_CREATED`, `EVT_WINDOW_DESTROYED`, and state updates to overlay clients.
- Minimize is implemented by setting `mapped = false` and selecting a fallback focused surface.

## Event dispatch and protocol handling

Client protocol handlers are implemented in `handle_client_message()`.

- `MSG_CREATE_SURFACE` allocates a surface and replies with the shared memory pathname.
- `MSG_RESIZE_SURFACE` allocates a new buffer and replies with the new path.
- `MSG_DAMAGE` triggers the compositor to finalize pending resize commits.
- `MSG_MOVE_SURFACE`, `MSG_SET_STATE`, `MSG_SET_TITLE`, and `MSG_SET_ICON` update surface metadata.
- `MSG_DESTROY_SURFACE` frees shared memory and removes the surface.
- `MSG_QUERY_WINDOWS` walks the surface list and replies with window information events.
- `MSG_QUIT` instructs the compositor to exit its main loop.

## Resizing strategy

Nova uses an optimistic resize workflow:

1. The compositor issues `EVT_RESIZE_REQUEST` to the client when the user resizes a window.
2. The client calls `nova_resize_surface()` to obtain a new shared memory buffer.
3. The client draws into the new buffer and calls `nova_damage_surface()`.
4. On the next `MSG_DAMAGE`, Nova swaps the pending buffer into the live surface.

Nova may also render a scaled preview during drag operations using `resize_preview_pixels`.

## Autostart and configuration

Nova parses `/etc/nova/nova.conf` for theme colors and autostart entries.

Common keys include:

- `active_titlebar_top`
- `active_titlebar_bottom`
- `inactive_titlebar_top`
- `inactive_titlebar_bottom`
- `active_border`
- `inactive_border`
- `taskbar`
- `wallpaperd`

Nova also responds to `SIGUSR1` by reloading the config file and refreshing the UI.

## Composition pipeline

The compositor uses a software backbuffer:

- Surfaces are composited into `back_buffer` in z-order.
- A cursor sprite is rendered last.
- Damage rectangles are flushed to the framebuffer via `copy_box_to_fb()`.
- If only the cursor moved, Nova can update a small region atomically instead of redrawing the full screen.

## Cleanup

When Nova exits:

- It closes the socket and removes `/tmp/nova.sock`.
- It unmapps the framebuffer and returns the console to `KD_TEXT`.
- It closes `/dev/fb0`.

## Notes for contributors

- Most window behaviors are hard-coded in `nova.c`; adding new window state flags requires both client and compositor support.
- The compositor currently expects surface resize and damage to occur in sequence.
- Overlay clients receive window list events through the same socket protocol.
- The compositor's input dispatch is deliberately simple and can be extended for drag-and-drop or touch.
