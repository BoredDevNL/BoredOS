#include "unix_socket.h"
#include "memory_manager.h"
#include "kutils.h"
#include "spinlock.h"
#include <string.h>

struct unix_listener {
    char path[108];
    int owner_pid;
    int owner_fd;
    int listening;
    struct unix_pending_conn *pending_head;
    spinlock_t lock;
    wait_queue_head_t accept_waitq;
    struct unix_listener *next;
};

static struct unix_listener *listeners = NULL;
static spinlock_t listeners_lock = SPINLOCK_INIT;

int unix_register_listener(const char *path, int owner_pid, int owner_fd) {
    if (!path) return -1;
    struct unix_listener *l = (struct unix_listener *)kmalloc(sizeof(*l));
    if (!l) return -1;
    memset(l,0,sizeof(*l));
    strncpy(l->path, path, sizeof(l->path)-1);
    l->owner_pid = owner_pid;
    l->owner_fd = owner_fd;
    l->listening = 0;
    wait_queue_init(&l->accept_waitq);
    l->pending_head = NULL;
    l->lock = SPINLOCK_INIT;

    uint64_t flags = spinlock_acquire_irqsave(&listeners_lock);
    l->next = listeners;
    listeners = l;
    spinlock_release_irqrestore(&listeners_lock, flags);
    return 0;
}

int unix_unregister_listener(const char *path) {
    if (!path) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&listeners_lock);
    struct unix_listener *prev = NULL;
    struct unix_listener *cur = listeners;
    while (cur) {
        if (strncmp(cur->path, path, sizeof(cur->path)) == 0) {
            if (prev) prev->next = cur->next; else listeners = cur->next;
            spinlock_release_irqrestore(&listeners_lock, flags);
            while (cur->pending_head) {
                struct unix_pending_conn *next = cur->pending_head->next;
                unix_free_pending(cur->pending_head);
                cur->pending_head = next;
            }
            kfree(cur);
            return 0;
        }
        prev = cur; cur = cur->next;
    }
    spinlock_release_irqrestore(&listeners_lock, flags);
    return -1;
}

unix_listener_t *unix_find_listener(const char *path) {
    if (!path) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&listeners_lock);
    struct unix_listener *cur = listeners;
    while (cur) {
        if (strncmp(cur->path, path, sizeof(cur->path)) == 0) {
            spinlock_release_irqrestore(&listeners_lock, flags);
            return cur;
        }
        cur = cur->next;
    }
    spinlock_release_irqrestore(&listeners_lock, flags);
    return NULL;
}

void unix_listener_set_listening(unix_listener_t *lst, int listening) {
    if (!lst) return;
    lst->listening = listening ? 1 : 0;
}

int unix_listener_is_listening(unix_listener_t *lst) {
    return lst ? lst->listening : 0;
}

unix_listener_t *unix_find_listener_by_owner(int owner_pid, int owner_fd) {
    uint64_t flags = spinlock_acquire_irqsave(&listeners_lock);
    struct unix_listener *cur = listeners;
    while (cur) {
        if (cur->owner_pid == owner_pid && cur->owner_fd == owner_fd) {
            spinlock_release_irqrestore(&listeners_lock, flags);
            return cur;
        }
        cur = cur->next;
    }
    spinlock_release_irqrestore(&listeners_lock, flags);
    return NULL;
}

int unix_enqueue_pending(unix_listener_t *lst, unix_pending_conn_t *pc) {
    if (!lst || !pc) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&lst->lock);
    pc->next = lst->pending_head;
    lst->pending_head = pc;
    spinlock_release_irqrestore(&lst->lock, flags);
    // wake up any accept waiters
    wait_queue_wake_all(&lst->accept_waitq);
    return 0;
}

unix_pending_conn_t *unix_dequeue_pending(unix_listener_t *lst) {
    if (!lst) return NULL;
    uint64_t flags = spinlock_acquire_irqsave(&lst->lock);
    unix_pending_conn_t *pc = lst->pending_head;
    if (pc) lst->pending_head = pc->next;
    spinlock_release_irqrestore(&lst->lock, flags);
    return pc;
}

wait_queue_head_t *unix_listener_get_accept_waitq(unix_listener_t *lst) {
    return lst ? &lst->accept_waitq : NULL;
}

int unix_listener_has_pending(unix_listener_t *lst) {
    if (!lst) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&lst->lock);
    int has = (lst->pending_head != NULL);
    spinlock_release_irqrestore(&lst->lock, flags);
    return has;
}

unix_pending_conn_t *unix_create_pending_conn(void *pipe1, void *pipe2, int client_pid, int client_fd) {
    unix_pending_conn_t *pc = (unix_pending_conn_t *)kmalloc(sizeof(*pc));
    if (!pc) return NULL;
    memset(pc,0,sizeof(*pc));
    pc->pipe1 = pipe1;
    pc->pipe2 = pipe2;
    pc->client_pid = client_pid;
    pc->client_fd = client_fd;
    pc->next = NULL;
    return pc;
}

void unix_free_pending(unix_pending_conn_t *pc) {
    if (!pc) return;
    kfree(pc);
}
