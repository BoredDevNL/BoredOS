// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "sockbuf.h"
#include "slab.h"
#include "kutils.h"

void sockbuf_init(sockbuf_t *sb, uint32_t hiwat) {
    if (!sb) return;
    sb->sb_cc = 0;
    sb->sb_hiwat = hiwat ? hiwat : (64 * 1024); // Default 64KB
    sb->sb_lowat = 1;
    sb->sb_flags = 0;
    sb->lock = SPINLOCK_INIT;
    wait_queue_init(&sb->waitq);
    sb->head = NULL;
    sb->tail = NULL;
}

void sockbuf_destroy(sockbuf_t *sb) {
    if (!sb) return;
    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);
    sockbuf_entry_t *curr = sb->head;
    while (curr) {
        sockbuf_entry_t *next = curr->next;
        if (curr->p) {
            pbuf_free(curr->p);
        }
        kfree_null(curr);
        curr = next;
    }
    sb->head = NULL;
    sb->tail = NULL;
    sb->sb_cc = 0;
    spinlock_release_irqrestore(&sb->lock, flags);
}

int sockbuf_append(sockbuf_t *sb, struct pbuf *p, const ip_addr_t *src_ip, uint16_t src_port) {
    if (!sb || !p) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);

    // Overflow check against high watermark
    if (sb->sb_cc + p->tot_len > sb->sb_hiwat) {
        spinlock_release_irqrestore(&sb->lock, flags);
        return -1; // Buffer full
    }

    sockbuf_entry_t *entry = (sockbuf_entry_t *)kmalloc(sizeof(sockbuf_entry_t));
    if (!entry) {
        spinlock_release_irqrestore(&sb->lock, flags);
        return -1;
    }

    pbuf_ref(p);
    entry->p = p;
    if (src_ip) entry->src_ip = *src_ip;
    else ip_addr_set_zero(&entry->src_ip);
    entry->src_port = src_port;
    entry->next = NULL;

    if (!sb->tail) {
        sb->head = entry;
        sb->tail = entry;
    } else {
        sb->tail->next = entry;
        sb->tail = entry;
    }

    sb->sb_cc += p->tot_len;
    spinlock_release_irqrestore(&sb->lock, flags);

    wait_queue_wake_all(&sb->waitq);
    return 0;
}

int sockbuf_read(sockbuf_t *sb, void *buf, size_t max_len, ip_addr_t *out_ip, uint16_t *out_port, int peek) {
    if (!sb || !buf || max_len == 0) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);

    size_t total_copied = 0;
    uint8_t *dest = (uint8_t *)buf;

    while (sb->head && total_copied < max_len) {
        sockbuf_entry_t *entry = sb->head;
        size_t avail = entry->p->tot_len;
        size_t wanted = max_len - total_copied;
        size_t to_copy = (wanted < avail) ? wanted : avail;
        if (to_copy > 0xFFFF) to_copy = 0xFFFF;

        pbuf_copy_partial(entry->p, dest + total_copied, (u16_t)to_copy, 0);

        if (total_copied == 0) {
            if (out_ip) *out_ip = entry->src_ip;
            if (out_port) *out_port = entry->src_port;
        }

        total_copied += to_copy;

        if (!peek) {
            if (to_copy == entry->p->tot_len) {
                sb->head = entry->next;
                if (!sb->head) sb->tail = NULL;
                sb->sb_cc -= entry->p->tot_len;
                pbuf_free(entry->p);
                kfree_null(entry);
            } else {
                struct pbuf *remainder = pbuf_free_header(entry->p, (u16_t)to_copy);
                entry->p = remainder;
                sb->sb_cc -= to_copy;
                break;
            }
        } else {
            break;
        }
    }

    spinlock_release_irqrestore(&sb->lock, flags);
    return (int)total_copied;
}

int sockbuf_is_empty(sockbuf_t *sb) {
    if (!sb) return 1;
    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);
    int empty = (sb->head == NULL);
    spinlock_release_irqrestore(&sb->lock, flags);
    return empty;
}

int sockbuf_readable(sockbuf_t *sb) {
    if (!sb) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);
    uint32_t threshold = sb->sb_lowat > 0 ? sb->sb_lowat : 1;
    int readable = (sb->sb_cc >= threshold);
    spinlock_release_irqrestore(&sb->lock, flags);
    return readable;
}

uint32_t sockbuf_get_cc(sockbuf_t *sb) {
    if (!sb) return 0;
    uint64_t flags = spinlock_acquire_irqsave(&sb->lock);
    uint32_t cc = sb->sb_cc;
    spinlock_release_irqrestore(&sb->lock, flags);
    return cc;
}
