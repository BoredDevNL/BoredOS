// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "pty.h"
#include "spinlock.h"
#include "wait_queue.h"
#include "kutils.h"
#include <stdbool.h>
#include <stdint.h>

static pty_pair_t g_ptys[PTY_MAX_COUNT];
static spinlock_t g_pty_global_lock = SPINLOCK_INIT;

static void pty_queue_init(pty_queue_t *q) {
    q->head = 0;
    q->tail = 0;
    wait_queue_init(&q->wait_queue);
    memset(q->buffer, 0, PTY_QUEUE_SIZE);
}

static void pty_queue_push(pty_queue_t *q, uint8_t val) {
    uint32_t next = (q->head + 1) % PTY_QUEUE_SIZE;
    if (next != q->tail) {
        q->buffer[q->head] = val;
        q->head = next;
        wait_queue_wake_all(&q->wait_queue);
    }
}

static int pty_queue_pop(pty_queue_t *q, uint8_t *buf, size_t len) {
    size_t count = 0;
    while (q->head != q->tail && count < len) {
        buf[count++] = q->buffer[q->tail];
        q->tail = (q->tail + 1) % PTY_QUEUE_SIZE;
    }
    return (int)count;
}

void pty_init(void) {
    for (int i = 0; i < PTY_MAX_COUNT; i++) {
        g_ptys[i].id = i;
        g_ptys[i].used = false;
        g_ptys[i].fg_pid = -1;
        g_ptys[i].lock = SPINLOCK_INIT;
    }
}

bool pty_is_pty_id(int id) {
    return id >= PTY_ID_BASE;
}

pty_pair_t* pty_get(int pty_id) {
    if (pty_id < PTY_ID_BASE) return NULL;
    int idx = pty_id - PTY_ID_BASE;
    if (idx < 0 || idx >= PTY_MAX_COUNT) return NULL;
    return &g_ptys[idx];
}

int pty_create(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_pty_global_lock);
    for (int i = 0; i < PTY_MAX_COUNT; i++) {
        if (!g_ptys[i].used) {
            g_ptys[i].used = true;
            g_ptys[i].fg_pid = -1;
            pty_queue_init(&g_ptys[i].master_to_slave);
            pty_queue_init(&g_ptys[i].slave_to_master);
            spinlock_release_irqrestore(&g_pty_global_lock, flags);
            return PTY_ID_BASE + i;
        }
    }
    spinlock_release_irqrestore(&g_pty_global_lock, flags);
    return -1;
}

int pty_destroy(int pty_id) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&g_pty_global_lock);
    p->used = false;
    p->fg_pid = -1;
    spinlock_release_irqrestore(&g_pty_global_lock, flags);
    return 0;
}

void pty_write_output(int pty_id, const char *data, size_t len) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return;
    for (size_t i = 0; i < len; i++) {
        pty_queue_push(&p->slave_to_master, (uint8_t)data[i]);
    }
}

int pty_read_output(int pty_id, char *buf, size_t len) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return 0;
    return pty_queue_pop(&p->slave_to_master, (uint8_t*)buf, len);
}

int pty_write_input(int pty_id, const char *buf, size_t len) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return 0;
    for (size_t i = 0; i < len; i++) {
        pty_queue_push(&p->master_to_slave, (uint8_t)buf[i]);
    }
    return (int)len;
}

int pty_read_input(int pty_id, char *buf, size_t len) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return 0;
    return pty_queue_pop(&p->master_to_slave, (uint8_t*)buf, len);
}

int pty_set_foreground(int pty_id, int pid) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p) return -1;
    p->fg_pid = pid;
    return 0;
}

int pty_get_foreground(int pty_id) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p) return -1;
    return p->fg_pid;
}

int pty_poll(int pty_id, struct poll_table *pt) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return 0;

    int mask = 0;
    if (pt && pt->qproc) {
        pt->qproc(&p->master_to_slave.wait_queue, pt);
    }

    if (p->master_to_slave.head != p->master_to_slave.tail) {
        mask |= 0x0001;
    }

    mask |= 0x0004;

    return mask;
}
