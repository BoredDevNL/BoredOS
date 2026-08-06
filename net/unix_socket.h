#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "wait_queue.h"
#include "spinlock.h"
#include "sockbuf.h"

#define UNP_STATE_UNCONNECTED 0
#define UNP_STATE_BOUND       1
#define UNP_STATE_LISTENING   2
#define UNP_STATE_CONNECTED   3
#define UNP_STATE_CLOSED      4

typedef struct unpcb {
    char path[108];
    uint8_t type;            // SOCK_STREAM or SOCK_DGRAM
    uint8_t state;           // UNP_STATE_*
    spinlock_t lock;
    struct unpcb *peer;      // Peer unpcb for stream or connected dgram
    void *sock;              // Backpointer to process_fd_socket_t

    // Accept queue for stream listener
    struct unpcb *accept_queue[16];
    int accept_count;
    wait_queue_head_t accept_waitq;

    // Ancillary passed file descriptor storage (SCM_RIGHTS)
    void *passed_objs[16];
    uint8_t passed_kinds[16];
    int passed_flags[16];
    int passed_fd_count;

    struct unpcb *next;      // Listener linked list
} unpcb_t;

int  unix_socket_create(void *sock, int type);
int  unix_socket_bind(void *sock, const char *path);
int  unix_socket_listen(void *sock, int backlog);
int  unix_socket_connect(void *sock, const char *path);
void* unix_socket_accept(void *sock, int nonblock);
int  unix_socket_send(void *sock, const void *data, size_t len, int nonblock, const int *pass_fds, int pass_fd_count, const char *dest_path);
int  unix_socket_recv(void *sock, void *data, size_t len, int nonblock, void **out_objs, uint8_t *out_kinds, int *out_flags, int *out_fd_count);
int  unix_socketpair(void *sock1, void *sock2, int type);
void unix_socket_close(void *sock);

#endif
