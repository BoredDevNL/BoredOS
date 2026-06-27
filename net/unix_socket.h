#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <stdint.h>
#include "wait_queue.h"

typedef struct unix_listener unix_listener_t;

typedef struct unix_pending_conn {
	void *pipe1; // client->server
	void *pipe2; // server->client
	int client_pid;
	int client_fd;
	struct unix_pending_conn *next;
} unix_pending_conn_t;

// Register a listener for a pathname. Returns 0 on success.
int unix_register_listener(const char *path, int owner_pid, int owner_fd);
// Unregister listener by pathname
int unix_unregister_listener(const char *path);
// Find listener by path; returns NULL if not found
unix_listener_t *unix_find_listener(const char *path);
// Find listener by owner pid and fd
unix_listener_t *unix_find_listener_by_owner(int owner_pid, int owner_fd);

void unix_listener_set_listening(unix_listener_t *lst, int listening);
int unix_listener_is_listening(unix_listener_t *lst);

// Enqueue a pending connection structure onto listener
int unix_enqueue_pending(unix_listener_t *lst, unix_pending_conn_t *pc);
// Dequeue a pending connection; returns NULL if none
unix_pending_conn_t *unix_dequeue_pending(unix_listener_t *lst);
wait_queue_head_t *unix_listener_get_accept_waitq(unix_listener_t *lst);
int unix_listener_has_pending(unix_listener_t *lst);

// Create a pending connection object (holds pipe pointers)
unix_pending_conn_t *unix_create_pending_conn(void *pipe1, void *pipe2, int client_pid, int client_fd);

// Release pending conn
void unix_free_pending(unix_pending_conn_t *pc);

#endif
