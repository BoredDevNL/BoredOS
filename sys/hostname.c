// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "hostname.h"
#include "spinlock.h"
#include "vfs.h"
#include "memory_manager.h"
#include "kutils.h"
#include <string.h>

static char g_kernel_hostname[MAX_HOSTNAME_LEN] = "boredos";
static spinlock_t hostname_lock = SPINLOCK_INIT;

void kernel_get_hostname(char *buf, size_t max_len) {
    if (!buf || max_len == 0) return;
    uint64_t flags = spinlock_acquire_irqsave(&hostname_lock);
    size_t len = strlen(g_kernel_hostname);
    if (len >= max_len) len = max_len - 1;
    memcpy(buf, g_kernel_hostname, len);
    buf[len] = '\0';
    spinlock_release_irqrestore(&hostname_lock, flags);
}

int kernel_set_hostname(const char *name, size_t len) {
    if (!name || len == 0) return -1;

    char temp[MAX_HOSTNAME_LEN];
    size_t copy_len = len;
    if (copy_len >= MAX_HOSTNAME_LEN) copy_len = MAX_HOSTNAME_LEN - 1;

    memcpy(temp, name, copy_len);
    temp[copy_len] = '\0';

    for (size_t i = 0; i < copy_len; i++) {
        if (temp[i] == '\n' || temp[i] == '\r' || temp[i] == ' ') {
            temp[i] = '\0';
            break;
        }
    }

    if (temp[0] == '\0') return -1;

    uint64_t flags = spinlock_acquire_irqsave(&hostname_lock);
    strncpy(g_kernel_hostname, temp, MAX_HOSTNAME_LEN - 1);
    g_kernel_hostname[MAX_HOSTNAME_LEN - 1] = '\0';
    spinlock_release_irqrestore(&hostname_lock, flags);

    return 0;
}

void hostname_init(void) {
    vfs_file_t *f = vfs_open("/etc/hostname", "r");
    if (!f) {
        f = vfs_open("/boot/etc/hostname", "r");
    }
    if (f) {
        char buf[MAX_HOSTNAME_LEN] = {0};
        int bytes = vfs_read(f, buf, sizeof(buf) - 1);
        if (bytes > 0) {
            buf[bytes] = '\0';
            kernel_set_hostname(buf, bytes);
        }
        vfs_close(f);
    }
}
