// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "file_index.h"
#include "vfs.h"
#include "memory_manager.h"
#include "spinlock.h"
#include <stddef.h>

static file_index_t g_file_index = {0};
static spinlock_t g_index_lock = SPINLOCK_INIT;
static bool g_index_valid = false;

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *d, const char *s) {
    while ((*d++ = *s++));
}

static int str_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void str_cat(char *d, const char *s) {
    while (*d) d++;
    str_copy(d, s);
}

static bool str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return false;
    }
    return true;
}

static int fuzzy_match_score(const char *query, const char *filename) {
    if (!query || !filename) return 0;
    
    int score = 0;
    int query_idx = 0;
    int consecutive = 0;
    
    for (int i = 0; filename[i] && query_idx < 256; i++) {
        char fc = filename[i];
        char qc = query[query_idx];
        
        if (fc >= 'A' && fc <= 'Z') fc += 32;
        if (qc >= 'A' && qc <= 'Z') qc += 32;
        
        if (fc == qc) {
            score += 10;
            consecutive++;
            if (consecutive > 1) score += 5;  
            query_idx++;
        } else {
            consecutive = 0;
        }
    }
    
    if (query_idx < str_len(query)) {
        return 0;
    }
    
    if (str_starts_with(filename, query)) {
        score += 20;
    }
    
    return score;
}

static void index_walk_directory(const char *path, int depth) {
    if (depth > 16 || g_file_index.count >= FILE_INDEX_MAX_ENTRIES) {
        return;  
    }
    
    if (str_starts_with(path, "/proc") || 
        str_starts_with(path, "/sys") ||
        str_starts_with(path, "/dev")) {
        return;
    }
    
    vfs_dirent_t *entries = (vfs_dirent_t *)kmalloc(sizeof(vfs_dirent_t) * 1024);
    if (!entries) {
        return; 
    }
    
    int count = vfs_list_directory(path, entries, 1024, 0);
    
    if (count <= 0 || count > 1024) {
        kfree(entries);
        return;
    }
    
    for (int i = 0; i < count; i++) {
        if (g_file_index.count >= FILE_INDEX_MAX_ENTRIES) {
            break;
        }
        
        vfs_dirent_t *entry = &entries[i];
        if (!entry) continue;
        
        
        if (!entry->name || entry->name[0] == 0) {
            continue;
        }
        
        
        if (str_cmp(entry->name, ".color") == 0 || 
            str_cmp(entry->name, ".origin") == 0 ||
            str_cmp(entry->name, ".") == 0 ||
            str_cmp(entry->name, "..") == 0) {
            continue;
        }
        
        char full_path[FILE_INDEX_MAX_PATH];
        int path_len = 0;
        
        
        for (int j = 0; path[j] && path_len < FILE_INDEX_MAX_PATH - 1; j++) {
            full_path[path_len++] = path[j];
        }
        
        
        if (path_len > 0 && full_path[path_len - 1] != '/' && path_len < FILE_INDEX_MAX_PATH - 1) {
            full_path[path_len++] = '/';
        }
        
        
        for (int j = 0; entry->name[j] && path_len < FILE_INDEX_MAX_PATH - 1; j++) {
            full_path[path_len++] = entry->name[j];
        }
        full_path[path_len] = 0;
        
        
        if (path_len >= FILE_INDEX_MAX_PATH - 1) {
            continue;
        }
        
        
        file_index_entry_t *idx_entry = &g_file_index.entries[g_file_index.count];
        str_copy(idx_entry->path, full_path);
        idx_entry->size = entry->size;
        idx_entry->mod_time_low = entry->write_date;
        idx_entry->mod_time_high = entry->write_time;
        idx_entry->is_directory = entry->is_directory;
        
        g_file_index.count++;
        
        
        if (entry->is_directory && !str_starts_with(full_path, "/proc") && 
            !str_starts_with(full_path, "/sys") && !str_starts_with(full_path, "/dev")) {
            index_walk_directory(full_path, depth + 1);
        }
    }
    
    
    kfree(entries);
}


void file_index_init(void) {
    g_file_index.count = 0;
    g_file_index.capacity = FILE_INDEX_MAX_ENTRIES;
    g_index_valid = false;
}


bool file_index_build(void) {
    
    
    
    
    g_file_index.count = 0;
    
    
    const char *safe_paths[] = {"/root", "/bin", "/Library", "/docs", NULL};
    
    
    for (int p = 0; safe_paths[p] != NULL && g_file_index.count < FILE_INDEX_MAX_ENTRIES; p++) {
        index_walk_directory(safe_paths[p], 0);
    }
    
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    g_index_valid = true;
    spinlock_release_irqrestore(&g_index_lock, flags);
    
    
    file_index_save();
    
    return true;
}


bool file_index_load(void) {
    vfs_file_t *file = vfs_open(FILE_INDEX_CACHE_PATH, "r");
    if (!file) {
        return false;  
    }
    
    
    uint32_t version = 0;
    if (vfs_read(file, &version, sizeof(version)) != sizeof(version)) {
        vfs_close(file);
        return false;
    }
    
    
    if (version != FILE_INDEX_VERSION) {
        vfs_close(file);
        return false;
    }
    
    
    int count = 0;
    if (vfs_read(file, &count, sizeof(count)) != sizeof(count)) {
        vfs_close(file);
        return false;
    }
    
    
    if (count < 0 || count > FILE_INDEX_MAX_ENTRIES) {
        vfs_close(file);
        return false;
    }
    
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    g_file_index.count = 0;
    g_index_valid = false;
    spinlock_release_irqrestore(&g_index_lock, flags);

    for (int i = 0; i < count; i++) {
        file_index_entry_t *entry = &g_file_index.entries[i];
        
        int bytes_read = vfs_read(file, entry->path, FILE_INDEX_MAX_PATH);
        if (bytes_read != FILE_INDEX_MAX_PATH) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_read(file, &entry->size, sizeof(entry->size)) != sizeof(entry->size)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_read(file, &entry->mod_time_low, sizeof(entry->mod_time_low)) != sizeof(entry->mod_time_low)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_read(file, &entry->mod_time_high, sizeof(entry->mod_time_high)) != sizeof(entry->mod_time_high)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_read(file, &entry->is_directory, sizeof(entry->is_directory)) != sizeof(entry->is_directory)) {
            vfs_close(file);
            return false;
        }
    }
    
    vfs_close(file);

    flags = spinlock_acquire_irqsave(&g_index_lock);
    g_file_index.count = count;
    g_index_valid = true;
    spinlock_release_irqrestore(&g_index_lock, flags);
    
    return true;
}

bool file_index_save(void) {
    if (!vfs_mkdir("/Library")) {
    }
    if (!vfs_mkdir("/Library/Index")) {
    }
    
    vfs_file_t *file = vfs_open(FILE_INDEX_CACHE_PATH, "w");
    if (!file) {
        return false;
    }
    
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    int count = g_file_index.count;
    spinlock_release_irqrestore(&g_index_lock, flags);
    
    
    uint32_t version = FILE_INDEX_VERSION;
    if (vfs_write(file, &version, sizeof(version)) != sizeof(version)) {
        vfs_close(file);
        return false;
    }
    
    if (vfs_write(file, &count, sizeof(count)) != sizeof(count)) {
        vfs_close(file);
        return false;
    }
    
    for (int i = 0; i < count; i++) {
        file_index_entry_t entry;
        flags = spinlock_acquire_irqsave(&g_index_lock);
        entry = g_file_index.entries[i];
        spinlock_release_irqrestore(&g_index_lock, flags);
        
        if (vfs_write(file, entry.path, FILE_INDEX_MAX_PATH) != FILE_INDEX_MAX_PATH) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_write(file, &entry.size, sizeof(entry.size)) != sizeof(entry.size)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_write(file, &entry.mod_time_low, sizeof(entry.mod_time_low)) != sizeof(entry.mod_time_low)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_write(file, &entry.mod_time_high, sizeof(entry.mod_time_high)) != sizeof(entry.mod_time_high)) {
            vfs_close(file);
            return false;
        }
        
        if (vfs_write(file, &entry.is_directory, sizeof(entry.is_directory)) != sizeof(entry.is_directory)) {
            vfs_close(file);
            return false;
        }
    }
    
    vfs_close(file);
    return true;
}

int file_index_find_fuzzy(const char *query, file_index_result_t *results, int max_results) {
    if (!query || !results || max_results <= 0) {
        return 0;
    }
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    
    int result_count = 0;
    
    for (int i = 0; i < g_file_index.count && result_count < max_results; i++) {
        if (i < 0 || i >= FILE_INDEX_MAX_ENTRIES) {
            break;
        }
        
        const char *path = g_file_index.entries[i].path;
        if (!path || path[0] == 0) {
            continue;
        }
        
        const char *filename = path;
        
        for (int j = 0; path[j]; j++) {
            if (path[j] == '/') {
                filename = &path[j + 1];
            }
        }
        
        if (!filename || filename[0] == 0) {
            continue;
        }
        
        int score = fuzzy_match_score(query, filename);
        
        if (score > 0) {
            results[result_count].entry = g_file_index.entries[i];
            results[result_count].match_score = score;
            result_count++;
        }
    }
    
    spinlock_release_irqrestore(&g_index_lock, flags);
    
    for (int i = 0; i < result_count; i++) {
        for (int j = i + 1; j < result_count; j++) {
            if (results[j].match_score > results[i].match_score) {
                file_index_result_t tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }
    
    return result_count;
}

bool file_index_add_entry(const char *path, uint32_t size, uint32_t mod_time_low, 
                         uint32_t mod_time_high, bool is_dir) {
    if (!path || g_file_index.count >= FILE_INDEX_MAX_ENTRIES) {
        return false;
    }
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    
    for (int i = 0; i < g_file_index.count; i++) {
        if (str_cmp(g_file_index.entries[i].path, path) == 0) {
            g_file_index.entries[i].size = size;
            g_file_index.entries[i].mod_time_low = mod_time_low;
            g_file_index.entries[i].mod_time_high = mod_time_high;
            spinlock_release_irqrestore(&g_index_lock, flags);
            return true;
        }
    }
    
    file_index_entry_t *entry = &g_file_index.entries[g_file_index.count];
    str_copy(entry->path, path);
    entry->size = size;
    entry->mod_time_low = mod_time_low;
    entry->mod_time_high = mod_time_high;
    entry->is_directory = is_dir;
    
    g_file_index.count++;
    
    spinlock_release_irqrestore(&g_index_lock, flags);
    return true;
}

bool file_index_remove_entry(const char *path) {
    if (!path) {
        return false;
    }
    
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    
    for (int i = 0; i < g_file_index.count; i++) {
        if (str_cmp(g_file_index.entries[i].path, path) == 0) {
            for (int j = i; j < g_file_index.count - 1; j++) {
                g_file_index.entries[j] = g_file_index.entries[j + 1];
            }
            g_file_index.count--;
            spinlock_release_irqrestore(&g_index_lock, flags);
            return true;
        }
    }
    
    spinlock_release_irqrestore(&g_index_lock, flags);
    return false;
}

int file_index_get_entry_count(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    int count = g_file_index.count;
    spinlock_release_irqrestore(&g_index_lock, flags);
    return count;
}

void file_index_clear(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    g_file_index.count = 0;
    g_index_valid = false;
    spinlock_release_irqrestore(&g_index_lock, flags);
}

void file_index_invalidate_cache(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    g_index_valid = false;
    spinlock_release_irqrestore(&g_index_lock, flags);
}

bool file_index_is_valid(void) {
    uint64_t flags = spinlock_acquire_irqsave(&g_index_lock);
    bool valid = g_index_valid;
    spinlock_release_irqrestore(&g_index_lock, flags);
    return valid;
}
