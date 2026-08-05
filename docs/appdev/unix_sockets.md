# Unix (AF_UNIX) Sockets

This document describes how AF_UNIX (local /domain) sockets are implemented in BoredOS, how to use the userland API (the usual BSD-style socket functions), and where the kernel-side implementation lives.

**Userland API:**
- `socket(AF_UNIX, SOCK_STREAM, 0)` creates a unix domain stream socket.
- `bind()` with `struct sockaddr_un` (path stored in `sun_path`) binds a socket to a filesystem pathname.
- `listen()` marks the socket as listening.
- `accept()` accepts an incoming connection; returns a new file descriptor for the connection.
- `connect()` connects to a listening pathname.
- `send()` / `recv()` (or `write()` / `read()`) exchanges data on a connected socket.
- `sendmsg()` / `recvmsg()` with `struct cmsghdr` (`SOL_SOCKET`, `SCM_RIGHTS`) passes open file descriptors across processes over UNIX domain sockets.

Notes:
- AF_UNIX paths are carried in `sockaddr_un.sun_path`; pathname length is limited by the `sun_path` buffer size.
- `bind()` in userland results in the kernel registering a listener for that pathname; `listen()` toggles the listener into a listening state.

Kernel internals (where to look):
- Header: [src/net/unix_socket.h](src/net/unix_socket.h#L1)
- Implementation: [src/net/unix_socket.c](src/net/unix_socket.c#L1)

Key kernel concepts and APIs:
- `unix_register_listener(const char *path, int owner_pid, int owner_fd)` — create and register a listener structure for `path`. The owner PID/FD identify the listening socket owner in the kernel.
- `unix_unregister_listener(const char *path)` — remove the listener and free pending connections.
- `unix_find_listener(const char *path)` and `unix_find_listener_by_owner(int owner_pid, int owner_fd)` — lookup helpers.
- `unix_listener_set_listening()` / `unix_listener_is_listening()` — flip and check the listening state.
- Pending connections are represented by `unix_pending_conn_t` (see `unix_socket.h`) which holds two pipe pointers (server<->client) and client metadata. The kernel enqueues pending connections with `unix_enqueue_pending()` and the listener dequeues them with `unix_dequeue_pending()` when the owner calls `accept()`.
- The listener struct contains a `wait_queue_head_t accept_waitq` that's used to wake up threads blocked in `accept()` when new pending connections arrive.

Behavior summary:
- When a userland socket calls `bind()` the kernel creates a listener entry for that path. When `listen()` is called the listener is marked listening.
- When a client `connect()`s, the kernel creates the necessary bidirectional pipes/streams, wraps them into a `unix_pending_conn_t`, and enqueues it on the listener's pending list. If the listener is not present or not listening, `connect()` fails.
- When the server calls `accept()`, the kernel dequeues a pending connection and hands back a new file descriptor wired to the server side of the pipe.

Implementation notes and caveats:
- Pathname length: listener `path` is stored in a fixed-size 108-byte buffer (see `unix_listener` in `unix_socket.c`). Userland should ensure paths fit.
- Pending connections are singly linked and freed when the listener is unregistered or after accept.
- The kernel uses spinlocks to protect listener lists and per-listener pending lists, and uses wait queues to block/wake acceptors.

If you want to extend or change behavior, inspect the socket layer code that invokes these unix socket helpers (the AF_UNIX socket bind/connect/accept paths in the kernel) and follow the `unix_register_listener` / `unix_enqueue_pending` call sites.

This page intentionally focuses on the AF_UNIX API contract and kernel implementation details rather than test/demo applications.
