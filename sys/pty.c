// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "pty.h"
#include "spinlock.h"
#include "wait_queue.h"
#include "kutils.h"
#include "process.h"
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
            g_ptys[i].ws.ws_row = 25;
            g_ptys[i].ws.ws_col = 80;
            g_ptys[i].ws.ws_xpixel = 640;
            g_ptys[i].ws.ws_ypixel = 200;
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
        uint8_t c = (uint8_t)buf[i];
        if (c == CTRL_C_CHAR) { // Ctrl+C (SIGINT)
            int fg = p->fg_pid;
            process_t *target = NULL;
            if (fg > 0) {
                target = process_get_by_pid((uint32_t)fg);
            }
            if (!target) {
                target = process_find_child_on_tty(pty_id);
            }
            if (target && target->pid > 1) {
                process_terminate_with_status(target, 128 + SIGINT_CODE);
                p->fg_pid = -1;
                continue;
            }
        }
        pty_queue_push(&p->master_to_slave, c);
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

int pty_poll_master(int pty_id, struct poll_table *pt) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return 0;

    int mask = 0;
    if (pt && pt->qproc) {
        pt->qproc(&p->slave_to_master.wait_queue, pt);
    }

    if (p->slave_to_master.head != p->slave_to_master.tail) {
        mask |= 0x0001;
    }

    mask |= 0x0004;

    return mask;
}

#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int pty_ioctl(int pty_id, uint64_t request, void *arg) {
    pty_pair_t *p = pty_get(pty_id);
    if (!p || !p->used) return -1;

    if (request == TIOCGWINSZ) {
        if (!arg) return -1;
        struct winsize *ws = (struct winsize *)arg;
        *ws = p->ws;
        return 0;
    } else if (request == TIOCSWINSZ) {
        if (!arg) return -1;
        struct winsize *ws = (struct winsize *)arg;
        p->ws = *ws;
        return 0;
    } else if (request == TIOCGPGRP) {
        if (!arg) return -1;
        *(int *)arg = p->fg_pid;
        return 0;
    } else if (request == TIOCSPGRP) {
        if (!arg) return -1;
        p->fg_pid = *(int *)arg;
        return 0;
    } else if (request == 0x80045430 || request == 0x5430) { // TIOCGPTN
        if (!arg) return -1;
        *(int *)arg = pty_id - PTY_ID_BASE;
        return 0;
    } else if (request == 0x40045431 || request == 0x5431) { // TIOCSPTLCK
        return 0;
    }

    return -1;
}

