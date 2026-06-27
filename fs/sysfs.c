#include "vfs.h"
#include "kernel_subsystem.h"
#include "memory_manager.h"
#include "kutils.h"

typedef struct {
    kernel_subsystem_t *sub;
    subsystem_file_t *file;
    int offset;
} sysfs_handle_t;

static void* sysfs_open(void *fs_private, const char *path, const char *mode) {
    if (path[0] == '/') path++;
    if (path[0] == '\0') return NULL;

    kernel_subsystem_t *sub = NULL;
    int last_slash = -1;
    for (int j = 0; path[j]; j++) if (path[j] == '/') last_slash = j;

    if (last_slash != -1) {
        char prefix[64];
        memcpy(prefix, path, last_slash);
        prefix[last_slash] = 0;
        sub = subsystem_get_by_name(prefix);
        
        if (sub) {
            const char *filename = path + last_slash + 1;
            for (int j = 0; j < sub->file_count; j++) {
                if (strcmp(sub->files[j].name, filename) == 0) {
                    sysfs_handle_t *h = (sysfs_handle_t*)kmalloc(sizeof(sysfs_handle_t));
                    h->sub = sub;
                    h->file = &sub->files[j];
                    h->offset = 0;
                    return h;
                }
            }
        }
    }

    return NULL;
}

static void sysfs_close(void *fs_private, void *handle) {
    if (handle) kfree(handle);
}

static int sysfs_read(void *fs_private, void *handle, void *buf, int size) {
    sysfs_handle_t *h = (sysfs_handle_t*)handle;
    if (!h || !h->file || !h->file->read) return -1;

    int bytes = h->file->read((char*)buf, size, h->offset);
    if (bytes > 0) h->offset += bytes;
    return bytes;
}

static int sysfs_write(void *fs_private, void *handle, const void *buf, int size) {
    sysfs_handle_t *h = (sysfs_handle_t*)handle;
    if (!h || !h->file || !h->file->write) return -1;

    int bytes = h->file->write((const char*)buf, size, h->offset);
    if (bytes > 0) h->offset += bytes;
    return bytes;
}

static int sysfs_readdir(void *fs_private, const char *path, vfs_dirent_t *entries, int max, int offset) {
    if (path[0] == '/') path++;
    
    kernel_subsystem_t *exact_sub = subsystem_get_by_name(path);
    int out = 0;
    int found_so_far = 0;

    if (exact_sub) {
        for (int i = 0; i < exact_sub->file_count && out < max; i++) {
            if (found_so_far >= offset) {
                strcpy(entries[out].name, exact_sub->files[i].name);
                entries[out].is_directory = 0;
                entries[out].size = 0;
                out++;
            }
            found_so_far++;
        }
    }

    int count = subsystem_get_count();
    int path_len = strlen(path);

    for (int i = 0; i < count && out < max; i++) {
        kernel_subsystem_t *s = subsystem_get_by_index(i);
        if (path_len == 0 || (strlen(s->name) > path_len && strncmp(s->name, path, path_len) == 0 && s->name[path_len] == '/')) {
            const char *sub_path = s->name + (path_len ? path_len + 1 : 0);
            char comp[64];
            int j = 0;
            while (sub_path[j] && sub_path[j] != '/' && j < 63) {
                comp[j] = sub_path[j];
                j++;
            }
            comp[j] = 0;

            if (comp[0] == '\0') continue;

            bool already_processed = false;
            for (int prev = 0; prev < i; prev++) {
                kernel_subsystem_t *ps = subsystem_get_by_index(prev);
                if (path_len == 0 || (strlen(ps->name) > path_len && strncmp(ps->name, path, path_len) == 0 && ps->name[path_len] == '/')) {
                    const char *p_sub_path = ps->name + (path_len ? path_len + 1 : 0);
                    char p_comp[64];
                    int pj = 0;
                    while (p_sub_path[pj] && p_sub_path[pj] != '/' && pj < 63) {
                        p_comp[pj] = p_sub_path[pj];
                        pj++;
                    }
                    p_comp[pj] = 0;
                    if (strcmp(p_comp, comp) == 0) {
                        already_processed = true;
                        break;
                    }
                }
            }

            if (!already_processed && exact_sub) {
                for (int f = 0; f < exact_sub->file_count; f++) {
                    if (strcmp(exact_sub->files[f].name, comp) == 0) {
                        already_processed = true;
                        break;
                    }
                }
            }

            if (already_processed) continue;

            if (found_so_far >= offset) {
                strcpy(entries[out].name, comp);
                entries[out].is_directory = 1;
                entries[out].size = 0;
                out++;
            }
            found_so_far++;
        }
    }
    return out;
}

static bool sysfs_exists(void *fs_private, const char *path) {
    if (path[0] == '/') path++;
    if (path[0] == '\0') return true;

    if (subsystem_get_by_name(path)) return true;

    // File check
    int last_slash = -1;
    for (int j = 0; path[j]; j++) if (path[j] == '/') last_slash = j;
    if (last_slash != -1) {
        char prefix[64];
        memcpy(prefix, path, last_slash);
        prefix[last_slash] = 0;
        kernel_subsystem_t *sub = subsystem_get_by_name(prefix);
        if (sub) {
            const char *filename = path + last_slash + 1;
            for (int j = 0; j < sub->file_count; j++) {
                if (strcmp(sub->files[j].name, filename) == 0) return true;
            }
        }
    }

    int count = subsystem_get_count();
    int path_len = strlen(path);
    for (int i = 0; i < count; i++) {
        kernel_subsystem_t *s = subsystem_get_by_index(i);
        if (strlen(s->name) > path_len && strncmp(s->name, path, path_len) == 0 && s->name[path_len] == '/') return true;
    }

    return false;
}

static bool sysfs_is_dir(void *fs_private, const char *path) {
    if (path[0] == '/') path++;
    if (path[0] == '\0') return true;

    int last_slash = -1;
    for (int j = 0; path[j]; j++) if (path[j] == '/') last_slash = j;
    if (last_slash != -1) {
        char prefix[64];
        memcpy(prefix, path, last_slash);
        prefix[last_slash] = 0;
        kernel_subsystem_t *sub = subsystem_get_by_name(prefix);
        if (sub) {
            const char *filename = path + last_slash + 1;
            for (int j = 0; j < sub->file_count; j++) {
                if (strcmp(sub->files[j].name, filename) == 0) return false;
            }
        }
    }

    return sysfs_exists(fs_private, path);
}

static int sysfs_statfs(void *fs_private, vfs_statfs_t *stat) {
    (void)fs_private;
    stat->total_blocks = 0;
    stat->free_blocks = 0;
    stat->block_size = 512;
    return 0;
}

vfs_fs_ops_t sysfs_ops = {
    .open = sysfs_open,
    .close = sysfs_close,
    .read = sysfs_read,
    .write = sysfs_write,
    .readdir = sysfs_readdir,
    .exists = sysfs_exists,
    .is_dir = sysfs_is_dir,
    .statfs = sysfs_statfs
};

vfs_fs_ops_t* sysfs_get_ops(void) {
    return &sysfs_ops;
}
