// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SOCKBUF_H
#define SOCKBUF_H
#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "wait_queue.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"

typedef struct sockbuf_entry {
    struct pbuf *p;
    ip_addr_t src_ip;
    uint16_t src_port;
    struct sockbuf_entry *next;
} sockbuf_entry_t;

typedef struct sockbuf {
    uint32_t sb_cc;       // Current byte count
    uint32_t sb_hiwat;    // High watermark (buffer cap)
    uint32_t sb_lowat;    // Low watermark
    uint8_t  sb_flags;    // Socket buffer flags
    spinlock_t lock;
    wait_queue_head_t waitq;
    sockbuf_entry_t *head;
    sockbuf_entry_t *tail;
} sockbuf_t;

void sockbuf_init(sockbuf_t *sb, uint32_t hiwat);
void sockbuf_destroy(sockbuf_t *sb);
int  sockbuf_append(sockbuf_t *sb, struct pbuf *p, const ip_addr_t *src_ip, uint16_t src_port);
int  sockbuf_read(sockbuf_t *sb, void *buf, size_t max_len, ip_addr_t *out_ip, uint16_t *out_port, int peek);
int  sockbuf_is_empty(sockbuf_t *sb);
int  sockbuf_readable(sockbuf_t *sb);
uint32_t sockbuf_get_cc(sockbuf_t *sb);

#endif
