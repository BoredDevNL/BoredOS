// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "tmpfs.h"
#include "slab.h"
#include "pmm.h"
#include "platform.h"
#include "kutils.h"
#include "spinlock.h"
#include <string.h>

static spinlock_t tmpfs_lock = SPINLOCK_INIT;
static tmpfs_inode_t *tmpfs_root = NULL;

static const char *tmpfs_strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return last;
}

static int tmpfs_aops_read_page(address_space_t *mapping, page_t *page) {
    (void)mapping;
    (void)page;
    return 0;
}

static int tmpfs_aops_write_page(address_space_t *mapping, page_t *page) {
    (void)mapping;
    (void)page;
    return 0;
}

static const address_space_ops_t tmpfs_aops = {
    .read_page = tmpfs_aops_read_page,
    .write_page = tmpfs_aops_write_page,
    .bmap = NULL,
};

static tmpfs_inode_t *tmpfs_alloc_inode(const char *name, bool is_dir) {
    tmpfs_inode_t *node = (tmpfs_inode_t *)kmalloc(sizeof(tmpfs_inode_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(tmpfs_inode_t));
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->is_dir = is_dir;
    address_space_init(&node->i_mapping, node, &tmpfs_aops);
    return node;
}

static tmpfs_inode_t *tmpfs_find_child(tmpfs_inode_t *dir, const char *name) {
    if (!dir || !dir->is_dir) return NULL;
    tmpfs_inode_t *cur = dir->children;
    while (cur) {
        if (strcmp(cur->name, name) == 0) return cur;
        cur = cur->next;
    }
    return NULL;
}

static void tmpfs_add_child(tmpfs_inode_t *parent, tmpfs_inode_t *child) {
    child->parent = parent;
    child->next = parent->children;
    parent->children = child;
}

static tmpfs_inode_t *tmpfs_lookup(const char *path, bool create_dirs, bool create_leaf, bool is_dir_leaf) {
    if (!path || path[0] == '\0') return tmpfs_root;
    if (strcmp(path, "/") == 0) return tmpfs_root;

    const char *p = path;
    if (*p == '/') p++;

    tmpfs_inode_t *cur = tmpfs_root;
    char segment[256];

    while (*p) {
        int seg_len = 0;
        while (*p && *p != '/' && seg_len < 255) {
            segment[seg_len++] = *p++;
        }
        segment[seg_len] = '\0';
        while (*p == '/') p++;

        bool is_last = (*p == '\0');
        tmpfs_inode_t *next = tmpfs_find_child(cur, segment);

        if (!next) {
            if (is_last && create_leaf) {
                next = tmpfs_alloc_inode(segment, is_dir_leaf);
                if (!next) return NULL;
                tmpfs_add_child(cur, next);
            } else if (!is_last && create_dirs) {
                next = tmpfs_alloc_inode(segment, true);
                if (!next) return NULL;
                tmpfs_add_child(cur, next);
            } else {
                return NULL;
            }
        }
        cur = next;
    }
    return cur;
}

static void *tmpfs_vfs_open(void *fs_private, const char *rel_path, const char *mode) {
    (void)fs_private;
    if (!rel_path || !mode) return NULL;

    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);

    bool write_mode = (mode[0] == 'w' || mode[0] == 'a');
    tmpfs_inode_t *inode = tmpfs_lookup(rel_path, write_mode, write_mode, false);
    if (!inode) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return NULL;
    }

    if (inode->is_dir && mode[0] != 'r') {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return NULL;
    }

    if (mode[0] == 'w' && !inode->is_dir) {
        pagecache_truncate_range(&inode->i_mapping, 0);
        inode->size = 0;
    }

    tmpfs_file_handle_t *handle = (tmpfs_file_handle_t *)kmalloc(sizeof(tmpfs_file_handle_t));
    if (!handle) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return NULL;
    }

    handle->inode = inode;
    handle->position = (mode[0] == 'a') ? inode->size : 0;
    handle->mode = (mode[0] == 'w') ? 1 : ((mode[0] == 'a') ? 2 : 0);

    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return handle;
}

static void tmpfs_vfs_close(void *fs_private, void *file_handle) {
    (void)fs_private;
    if (file_handle) kfree(file_handle);
}

static int tmpfs_vfs_read(void *fs_private, void *file_handle, void *buf, size_t size) {
    (void)fs_private;
    if (!file_handle || !buf || size == 0) return 0;
    tmpfs_file_handle_t *handle = (tmpfs_file_handle_t *)file_handle;
    tmpfs_inode_t *inode = handle->inode;
    if (!inode || inode->is_dir) return -1;

    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    if (handle->position >= inode->size) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return 0;
    }

    size_t to_read = size;
    if (handle->position + to_read > inode->size) {
        to_read = inode->size - handle->position;
    }
    spinlock_release_irqrestore(&tmpfs_lock, flags);

    size_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buf;

    while (bytes_read < to_read) {
        uint32_t pos = handle->position + (uint32_t)bytes_read;
        uint64_t index = pos >> 12;
        uint32_t offset_in_page = pos & 4095;
        size_t chunk = 4096 - offset_in_page;
        if (chunk > (to_read - bytes_read)) chunk = to_read - bytes_read;

        page_t *p = pagecache_get_page(&inode->i_mapping, index);
        if (!p) break;

        uintptr_t phys = pmm_page_to_paddr(p);
        memcpy(dst + bytes_read, (const void *)p2v(phys + offset_in_page), chunk);
        pmm_free_page(p);
        bytes_read += chunk;
    }

    handle->position += (uint32_t)bytes_read;
    return (int)bytes_read;
}

static int tmpfs_vfs_write(void *fs_private, void *file_handle, const void *buf, size_t size) {
    (void)fs_private;
    if (!file_handle || !buf || size == 0) return 0;
    tmpfs_file_handle_t *handle = (tmpfs_file_handle_t *)file_handle;
    tmpfs_inode_t *inode = handle->inode;
    if (!inode || inode->is_dir) return -1;

    size_t bytes_written = 0;
    const uint8_t *src = (const uint8_t *)buf;

    while (bytes_written < size) {
        uint32_t pos = handle->position + (uint32_t)bytes_written;
        uint64_t index = pos >> 12;
        uint32_t offset_in_page = pos & 4095;
        size_t chunk = 4096 - offset_in_page;
        if (chunk > (size - bytes_written)) chunk = size - bytes_written;

        page_t *p = pagecache_get_page(&inode->i_mapping, index);
        if (!p) break;

        uintptr_t phys = pmm_page_to_paddr(p);
        memcpy((void *)p2v(phys + offset_in_page), src + bytes_written, chunk);
        pmm_free_page(p);
        bytes_written += chunk;
    }

    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    handle->position += (uint32_t)bytes_written;
    if (handle->position > inode->size) {
        inode->size = handle->position;
    }
    spinlock_release_irqrestore(&tmpfs_lock, flags);

    return (int)bytes_written;
}

static int tmpfs_vfs_seek(void *fs_private, void *file_handle, int offset, int whence) {
    (void)fs_private;
    if (!file_handle) return -1;
    tmpfs_file_handle_t *handle = (tmpfs_file_handle_t *)file_handle;

    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    int new_pos = handle->position;
    if (whence == 0) new_pos = offset;
    else if (whence == 1) new_pos += offset;
    else if (whence == 2) new_pos = (int)handle->inode->size + offset;

    if (new_pos < 0) new_pos = 0;
    handle->position = (uint32_t)new_pos;
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return new_pos;
}

static int tmpfs_vfs_readdir(void *fs_private, const char *rel_path, vfs_dirent_t *entries, int max, int offset) {
    (void)fs_private;
    if (!entries || max <= 0) return 0;

    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *dir = tmpfs_lookup(rel_path, false, false, false);
    if (!dir || !dir->is_dir) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return 0;
    }

    int count = 0;
    int idx = 0;
    tmpfs_inode_t *cur = dir->children;
    while (cur && count < max) {
        if (idx >= offset) {
            strncpy(entries[count].name, cur->name, sizeof(entries[count].name) - 1);
            entries[count].size = cur->size;
            entries[count].is_directory = cur->is_dir;
            entries[count].start_cluster = 0;
            entries[count].write_date = 0;
            entries[count].write_time = 0;
            count++;
        }
        idx++;
        cur = cur->next;
    }

    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return count;
}

static bool tmpfs_vfs_mkdir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    if (!rel_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, true, true, true);
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return (node != NULL);
}

static bool tmpfs_vfs_rmdir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    if (!rel_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, false, false, false);
    if (!node || !node->is_dir || node->children != NULL || !node->parent) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return false;
    }

    tmpfs_inode_t *prev = NULL;
    tmpfs_inode_t *cur = node->parent->children;
    while (cur) {
        if (cur == node) {
            if (prev) prev->next = cur->next;
            else node->parent->children = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    address_space_destroy(&node->i_mapping);
    kfree(node);
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return true;
}

static bool tmpfs_vfs_unlink(void *fs_private, const char *rel_path) {
    (void)fs_private;
    if (!rel_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, false, false, false);
    if (!node || node->is_dir || !node->parent) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return false;
    }

    tmpfs_inode_t *prev = NULL;
    tmpfs_inode_t *cur = node->parent->children;
    while (cur) {
        if (cur == node) {
            if (prev) prev->next = cur->next;
            else node->parent->children = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    address_space_destroy(&node->i_mapping);
    kfree(node);
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return true;
}

static bool tmpfs_vfs_rename(void *fs_private, const char *old_path, const char *new_path) {
    (void)fs_private;
    if (!old_path || !new_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *old_node = tmpfs_lookup(old_path, false, false, false);
    if (!old_node || !old_node->parent) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return false;
    }

    const char *last_slash = tmpfs_strrchr(new_path, '/');
    char new_parent_path[256] = "";
    const char *new_name = new_path;

    if (last_slash) {
        size_t plen = (size_t)(last_slash - new_path);
        if (plen >= sizeof(new_parent_path)) plen = sizeof(new_parent_path) - 1;
        strncpy(new_parent_path, new_path, plen);
        new_parent_path[plen] = '\0';
        new_name = last_slash + 1;
    }

    tmpfs_inode_t *new_parent = tmpfs_lookup(new_parent_path, false, false, false);
    if (!new_parent || !new_parent->is_dir) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return false;
    }

    tmpfs_inode_t *existing = tmpfs_find_child(new_parent, new_name);
    if (existing == old_node) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return true;
    }
    if (existing) {
        if (existing->is_dir) {
            spinlock_release_irqrestore(&tmpfs_lock, flags);
            return false;
        }
        tmpfs_inode_t *prev = NULL;
        tmpfs_inode_t *cur = new_parent->children;
        while (cur) {
            if (cur == existing) {
                if (prev) prev->next = cur->next;
                else new_parent->children = cur->next;
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        address_space_destroy(&existing->i_mapping);
        kfree(existing);
    }

    tmpfs_inode_t *prev = NULL;
    tmpfs_inode_t *cur = old_node->parent->children;
    while (cur) {
        if (cur == old_node) {
            if (prev) prev->next = cur->next;
            else old_node->parent->children = cur->next;
            break;
        }
        prev = cur;
        cur = cur->next;
    }

    strncpy(old_node->name, new_name, sizeof(old_node->name) - 1);
    tmpfs_add_child(new_parent, old_node);

    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return true;
}

static bool tmpfs_vfs_exists(void *fs_private, const char *rel_path) {
    (void)fs_private;
    if (!rel_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, false, false, false);
    bool exists = (node != NULL);
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return exists;
}

static bool tmpfs_vfs_is_dir(void *fs_private, const char *rel_path) {
    (void)fs_private;
    if (!rel_path) return false;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, false, false, false);
    bool is_dir = node ? node->is_dir : false;
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return is_dir;
}

static int tmpfs_vfs_get_info(void *fs_private, const char *rel_path, vfs_dirent_t *info) {
    (void)fs_private;
    if (!rel_path || !info) return -1;
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    tmpfs_inode_t *node = tmpfs_lookup(rel_path, false, false, false);
    if (!node) {
        spinlock_release_irqrestore(&tmpfs_lock, flags);
        return -1;
    }
    strncpy(info->name, node->name, sizeof(info->name) - 1);
    info->size = node->size;
    info->is_directory = node->is_dir;
    info->start_cluster = 0;
    info->write_date = 0;
    info->write_time = 0;
    spinlock_release_irqrestore(&tmpfs_lock, flags);
    return 0;
}

static uint32_t tmpfs_vfs_get_position(void *file_handle) {
    if (!file_handle) return 0;
    return ((tmpfs_file_handle_t *)file_handle)->position;
}

static uint32_t tmpfs_vfs_get_size(void *file_handle) {
    if (!file_handle) return 0;
    tmpfs_file_handle_t *h = (tmpfs_file_handle_t *)file_handle;
    return h->inode ? h->inode->size : 0;
}

static int tmpfs_vfs_statfs(void *fs_private, vfs_statfs_t *stat) {
    (void)fs_private;
    if (!stat) return -1;
    stat->total_blocks = 1048576;
    stat->free_blocks = 1048576;
    stat->block_size = 4096;
    return 0;
}

static vfs_fs_ops_t tmpfs_ops = {
    .open = tmpfs_vfs_open,
    .close = tmpfs_vfs_close,
    .read = tmpfs_vfs_read,
    .write = tmpfs_vfs_write,
    .seek = tmpfs_vfs_seek,
    .readdir = tmpfs_vfs_readdir,
    .mkdir = tmpfs_vfs_mkdir,
    .rmdir = tmpfs_vfs_rmdir,
    .unlink = tmpfs_vfs_unlink,
    .rename = tmpfs_vfs_rename,
    .exists = tmpfs_vfs_exists,
    .is_dir = tmpfs_vfs_is_dir,
    .get_info = tmpfs_vfs_get_info,
    .statfs = tmpfs_vfs_statfs,
    .get_position = tmpfs_vfs_get_position,
    .get_size = tmpfs_vfs_get_size,
    .poll = NULL,
    .ioctl = NULL,
};

void tmpfs_init(void) {
    if (!tmpfs_root) {
        tmpfs_root = tmpfs_alloc_inode("", true);
    }
}

static void tmpfs_free_tree(tmpfs_inode_t *node) {
    if (!node) return;
    tmpfs_inode_t *child = node->children;
    while (child) {
        tmpfs_inode_t *next = child->next;
        tmpfs_free_tree(child);
        child = next;
    }
    pagecache_truncate_range(&node->i_mapping, 0);
    address_space_destroy(&node->i_mapping);
    kfree(node);
}

void tmpfs_destroy_all(void) {
    uint64_t flags = spinlock_acquire_irqsave(&tmpfs_lock);
    if (tmpfs_root) {
        tmpfs_free_tree(tmpfs_root);
        tmpfs_root = NULL;
    }
    spinlock_release_irqrestore(&tmpfs_lock, flags);
}

vfs_fs_ops_t *tmpfs_get_ops(void) {
    return &tmpfs_ops;
}
