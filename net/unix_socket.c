#include "unix_socket.h"
#include "process.h"
#include "slab.h"
#include "kutils.h"
#include "spinlock.h"
#include <string.h>

static unpcb_t *unix_listeners = NULL;
static spinlock_t unix_listeners_lock = SPINLOCK_INIT;

int unix_socket_create(void *s, int type) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock) return -1;

    unpcb_t *unp = (unpcb_t *)kmalloc(sizeof(unpcb_t));
    if (!unp) return -1;
    memset(unp, 0, sizeof(*unp));

    unp->type = (uint8_t)type;
    unp->state = UNP_STATE_UNCONNECTED;
    unp->lock = SPINLOCK_INIT;
    unp->peer = NULL;
    unp->sock = sock;
    unp->accept_count = 0;
    wait_queue_init(&unp->accept_waitq);
    unp->passed_fd_count = 0;
    unp->next = NULL;

    sock->unpcb = unp;
    return 0;
}

int unix_socket_bind(void *s, const char *path) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !sock->unpcb || !path) return -1;

    unpcb_t *unp = (unpcb_t *)sock->unpcb;
    uint64_t flags = spinlock_acquire_irqsave(&unp->lock);
    if (unp->state != UNP_STATE_UNCONNECTED) {
        spinlock_release_irqrestore(&unp->lock, flags);
        return -1;
    }

    strncpy(unp->path, path, sizeof(unp->path) - 1);
    unp->state = UNP_STATE_BOUND;
    spinlock_release_irqrestore(&unp->lock, flags);

    flags = spinlock_acquire_irqsave(&unix_listeners_lock);
    unp->next = unix_listeners;
    unix_listeners = unp;
    spinlock_release_irqrestore(&unix_listeners_lock, flags);

    sock->is_bound = 1;
    strncpy(sock->path, path, sizeof(sock->path) - 1);
    return 0;
}

int unix_socket_listen(void *s, int backlog) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !sock->unpcb) return -1;

    unpcb_t *unp = (unpcb_t *)sock->unpcb;
    uint64_t flags = spinlock_acquire_irqsave(&unp->lock);
    if (unp->state != UNP_STATE_BOUND) { // must be bound
        spinlock_release_irqrestore(&unp->lock, flags);
        return -1;
    }
    unp->state = UNP_STATE_LISTENING;
    spinlock_release_irqrestore(&unp->lock, flags);

    if (backlog > 0) {
        sock->backlog_max = backlog > 16 ? 16 : (uint32_t)backlog;
    }

    sock->is_listening = 1;
    return 0;
}

int unix_socket_connect(void *s, const char *path) {
    process_fd_socket_t *client_sock = (process_fd_socket_t *)s;
    if (!client_sock || !client_sock->unpcb || !path) return -1;

    unpcb_t *client_unp = (unpcb_t *)client_sock->unpcb;

    // Find listener
    uint64_t lflags = spinlock_acquire_irqsave(&unix_listeners_lock);
    unpcb_t *server_unp = unix_listeners;
    while (server_unp) {
        if (strncmp(server_unp->path, path, sizeof(server_unp->path)) == 0) {
            break;
        }
        server_unp = server_unp->next;
    }
    spinlock_release_irqrestore(&unix_listeners_lock, lflags);

    if (!server_unp || server_unp->state != UNP_STATE_LISTENING) {
        return -1; // Not listening
    }

    // Create server-side peer socket for accepted connection
    process_fd_socket_t *server_conn_sock = process_socket_create();
    if (!server_conn_sock) return -1;
    server_conn_sock->domain = 1;
    server_conn_sock->type = client_sock->type;
    unix_socket_create(server_conn_sock, client_sock->type);

    unpcb_t *server_conn_unp = (unpcb_t *)server_conn_sock->unpcb;

    // Connect peers symmetrically
    client_unp->peer = server_conn_unp;
    server_conn_unp->peer = client_unp;
    client_unp->state = UNP_STATE_CONNECTED;
    server_conn_unp->state = UNP_STATE_CONNECTED;

    client_sock->is_connected = 1;
    server_conn_sock->is_connected = 1;
    server_conn_sock->is_bound = 1;

    // Enqueue server_conn_sock onto listener's accept queue respecting backlog cap
    process_fd_socket_t *server_listener_sock = (process_fd_socket_t *)server_unp->sock;
    int max_backlog = (server_listener_sock && server_listener_sock->backlog_max > 0 && server_listener_sock->backlog_max <= 16) ? server_listener_sock->backlog_max : 16;

    uint64_t sflags = spinlock_acquire_irqsave(&server_unp->lock);
    if (server_unp->accept_count < max_backlog) {
        server_unp->accept_queue[server_unp->accept_count++] = server_conn_unp;
        spinlock_release_irqrestore(&server_unp->lock, sflags);
        wait_queue_wake_all(&server_unp->accept_waitq);
        if (server_listener_sock) {
            wait_queue_wake_all(&server_listener_sock->accept_waitq);
        }
        return 0;
    } else {
        spinlock_release_irqrestore(&server_unp->lock, sflags);
        process_socket_release(server_conn_sock);
        return -1;
    }
}

void* unix_socket_accept(void *s, int nonblock) {
    process_fd_socket_t *listener_sock = (process_fd_socket_t *)s;
    if (!listener_sock || !listener_sock->unpcb) return NULL;

    unpcb_t *listener_unp = (unpcb_t *)listener_sock->unpcb;

    while (1) {
        uint64_t flags = spinlock_acquire_irqsave(&listener_unp->lock);
        if (listener_unp->accept_count > 0) {
            unpcb_t *client_unp = listener_unp->accept_queue[0];
            for (int i = 1; i < listener_unp->accept_count; i++) {
                listener_unp->accept_queue[i - 1] = listener_unp->accept_queue[i];
            }
            listener_unp->accept_count--;
            listener_unp->accept_queue[listener_unp->accept_count] = NULL;
            spinlock_release_irqrestore(&listener_unp->lock, flags);

            return client_unp ? client_unp->sock : NULL;
        }
        spinlock_release_irqrestore(&listener_unp->lock, flags);

        if (nonblock) return NULL;
        wait_queue_wait(&listener_unp->accept_waitq);
    }
}

int unix_socketpair(void *s1, void *s2, int type) {
    process_fd_socket_t *sock1 = (process_fd_socket_t *)s1;
    process_fd_socket_t *sock2 = (process_fd_socket_t *)s2;

    if (!sock1 || !sock2) return -1;

    sock1->domain = 1; sock1->type = type;
    sock2->domain = 1; sock2->type = type;

    if (unix_socket_create(sock1, type) < 0) return -1;
    if (unix_socket_create(sock2, type) < 0) return -1;

    unpcb_t *unp1 = (unpcb_t *)sock1->unpcb;
    unpcb_t *unp2 = (unpcb_t *)sock2->unpcb;

    unp1->peer = unp2;
    unp2->peer = unp1;

    unp1->state = UNP_STATE_CONNECTED;
    unp2->state = UNP_STATE_CONNECTED;

    sock1->is_connected = 1;
    sock2->is_connected = 1;
    return 0;
}

int unix_socket_send(void *s, const void *data, size_t len, int nonblock, const int *pass_fds, int pass_fd_count, const char *dest_path) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !sock->unpcb) return -1;

    unpcb_t *unp = (unpcb_t *)sock->unpcb;
    unpcb_t *peer_unp = unp->peer;

    if (!peer_unp && unp->type == 2 && dest_path) {
        uint64_t lflags = spinlock_acquire_irqsave(&unix_listeners_lock);
        unpcb_t *curr = unix_listeners;
        while (curr) {
            if (strncmp(curr->path, dest_path, sizeof(curr->path)) == 0) {
                peer_unp = curr;
                break;
            }
            curr = curr->next;
        }
        spinlock_release_irqrestore(&unix_listeners_lock, lflags);
    }

    if (!peer_unp) return -1;

    process_fd_socket_t *peer_sock = (process_fd_socket_t *)peer_unp->sock;
    if (!peer_sock) return -1;

    // Pass file descriptors via SCM_RIGHTS by duplicating open file handles
    if (pass_fds && pass_fd_count > 0) {
        extern process_t *process_get_current(void);
        process_t *cur_proc = process_get_current();
        if (cur_proc) {
            uint64_t pflags = spinlock_acquire_irqsave(&peer_unp->lock);
            for (int i = 0; i < pass_fd_count && peer_unp->passed_fd_count < 16; i++) {
                int src_fd = pass_fds[i];
                if (src_fd >= 0 && src_fd < MAX_PROCESS_FDS && cur_proc->fds[src_fd]) {
                    peer_unp->passed_objs[peer_unp->passed_fd_count] = cur_proc->fds[src_fd];
                    peer_unp->passed_kinds[peer_unp->passed_fd_count] = cur_proc->fd_kind[src_fd];
                    peer_unp->passed_flags[peer_unp->passed_fd_count] = cur_proc->fd_flags[src_fd];

                    if (cur_proc->fd_kind[src_fd] == PROC_FD_KIND_FILE) {
                        process_fd_file_ref_t *ref = (process_fd_file_ref_t *)cur_proc->fds[src_fd];
                        if (ref) ref->refs++;
                    } else if (cur_proc->fd_kind[src_fd] == PROC_FD_KIND_PIPE_READ) {
                        process_fd_pipe_t *pipe = (process_fd_pipe_t *)cur_proc->fds[src_fd];
                        if (pipe) pipe->readers++;
                    } else if (cur_proc->fd_kind[src_fd] == PROC_FD_KIND_PIPE_WRITE) {
                        process_fd_pipe_t *pipe = (process_fd_pipe_t *)cur_proc->fds[src_fd];
                        if (pipe) pipe->writers++;
                    } else if (cur_proc->fd_kind[src_fd] == PROC_FD_KIND_SOCKET) {
                        process_socket_addref((process_fd_socket_t *)cur_proc->fds[src_fd]);
                    }
                    peer_unp->passed_fd_count++;
                }
            }
            spinlock_release_irqrestore(&peer_unp->lock, pflags);
        }
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
    if (!p) return -1;
    memcpy(p->payload, data, len);

    if (sockbuf_append(&peer_sock->rx_sb, p, NULL, 0) < 0) {
        pbuf_free(p);
        return -2;
    }
    return (int)len;
}

int unix_socket_recv(void *s, void *data, size_t len, int nonblock, void **out_objs, uint8_t *out_kinds, int *out_flags, int *out_fd_count) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !sock->unpcb) return -1;

    unpcb_t *unp = (unpcb_t *)sock->unpcb;

    while (sockbuf_is_empty(&sock->rx_sb)) {
        if (unp->state == UNP_STATE_CLOSED || (unp->peer == NULL && unp->state != UNP_STATE_LISTENING)) return 0; // Connection closed
        if (nonblock) return -2; // EWOULDBLOCK
        wait_queue_wait(&sock->rx_sb.waitq);
    }

    int copied = sockbuf_read(&sock->rx_sb, data, len, NULL, NULL, 0);

    // Retrieve passed descriptors if requested
    if (out_fd_count) {
        uint64_t flags = spinlock_acquire_irqsave(&unp->lock);
        int cnt = unp->passed_fd_count;
        if (cnt > 0) {
            for (int i = 0; i < cnt; i++) {
                if (out_objs) out_objs[i] = unp->passed_objs[i];
                if (out_kinds) out_kinds[i] = unp->passed_kinds[i];
                if (out_flags) out_flags[i] = unp->passed_flags[i];
                unp->passed_objs[i] = NULL;
            }
            *out_fd_count = cnt;
            unp->passed_fd_count = 0;
        } else {
            *out_fd_count = 0;
        }
        spinlock_release_irqrestore(&unp->lock, flags);
    }

    return copied;
}

void unix_socket_close(void *s) {
    process_fd_socket_t *sock = (process_fd_socket_t *)s;
    if (!sock || !sock->unpcb) return;

    unpcb_t *unp = (unpcb_t *)sock->unpcb;
    uint8_t old_state = unp->state;
    unp->state = UNP_STATE_CLOSED;

    // Unlink from listeners if was bound or listening
    if (old_state == UNP_STATE_BOUND || old_state == UNP_STATE_LISTENING) {
        uint64_t lflags = spinlock_acquire_irqsave(&unix_listeners_lock);
        unpcb_t *prev = NULL;
        unpcb_t *curr = unix_listeners;
        while (curr) {
            if (curr == unp) {
                if (prev) prev->next = curr->next;
                else unix_listeners = curr->next;
                break;
            }
            prev = curr;
            curr = curr->next;
        }
        spinlock_release_irqrestore(&unix_listeners_lock, lflags);
    }

    // Break peer connection
    if (unp->peer) {
        unpcb_t *saved_peer = unp->peer;
        unp->peer = NULL;
        uint64_t pflags = spinlock_acquire_irqsave(&saved_peer->lock);
        saved_peer->peer = NULL;
        process_fd_socket_t *peer_sock = saved_peer->sock ? (process_fd_socket_t *)saved_peer->sock : NULL;
        spinlock_release_irqrestore(&saved_peer->lock, pflags);
        if (peer_sock) {
            wait_queue_wake_all(&peer_sock->rx_sb.waitq);
        }
    }

    wait_queue_wake_all(&sock->rx_sb.waitq);

    kfree_null(unp);
    sock->unpcb = NULL;
}
