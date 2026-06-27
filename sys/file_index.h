// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef FILE_INDEX_H
#define FILE_INDEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FILE_INDEX_MAX_ENTRIES 50000
#define FILE_INDEX_MAX_PATH 1024
#define FILE_INDEX_CACHE_PATH "/Library/Index/file_index.dat"
#define FILE_INDEX_VERSION 1

typedef struct {
    char path[FILE_INDEX_MAX_PATH];
    uint32_t size;
    uint32_t mod_time_low; 
    uint32_t mod_time_high;
    bool is_directory;
} file_index_entry_t;

typedef struct {
    file_index_entry_t entry;
    int match_score;  
} file_index_result_t;

typedef struct {
    file_index_entry_t entries[FILE_INDEX_MAX_ENTRIES];
    int count;         
    int capacity;     
} file_index_t;

void file_index_init(void);
bool file_index_build(void);
bool file_index_load(void);
bool file_index_save(void);

int file_index_find_fuzzy(const char *query, file_index_result_t *results, int max_results);

bool file_index_add_entry(const char *path, uint32_t size, uint32_t mod_time_low, uint32_t mod_time_high, bool is_dir);
bool file_index_remove_entry(const char *path);

int file_index_get_entry_count(void);
void file_index_clear(void);
void file_index_invalidate_cache(void);
bool file_index_is_valid(void);  

#endif
