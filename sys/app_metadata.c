// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "app_metadata.h"

#include "memory_manager.h"
#include "vfs.h"
#include "kutils.h"


#define APP_METADATA_CACHE_SIZE 64

typedef struct {
    bool valid;
    bool has_metadata;
    char path[VFS_MAX_PATH];
    boredos_app_metadata_t metadata;
} app_metadata_cache_entry_t;

static app_metadata_cache_entry_t g_app_metadata_cache[APP_METADATA_CACHE_SIZE];
static int g_app_metadata_cache_next = 0;

static size_t am_strlen(const char *str) {
    size_t len = 0;
    if (!str) return 0;
    while (str[len]) len++;
    return len;
}

static bool am_str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static bool am_mem_eq(const uint8_t *a, const uint8_t *b, size_t len) {
    if (!a || !b) return false;
    return memcmp(a, b, len) == 0;
}

static void am_mem_copy(uint8_t *dest, const uint8_t *src, size_t len) {
    if (!dest || !src) return;
    memcpy(dest, src, len);
}

static void am_str_copy(char *dest, const char *src, size_t dest_size) {
    size_t i = 0;
    if (!dest || dest_size == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }

    while (src[i] && i + 1 < dest_size) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static bool am_seek(vfs_file_t *file, uint64_t offset) {
    if (!file) return false;
    if (offset > 0x7FFFFFFFUL) return false;
    return vfs_seek(file, (int)offset, 0) == 0;
}

static bool am_read_exact(vfs_file_t *file, void *buf, uint32_t size) {
    uint8_t *dst = (uint8_t *)buf;
    uint32_t total = 0;

    while (total < size) {
        int rc = vfs_read(file, dst + total, (int)(size - total));
        if (rc <= 0) return false;
        total += (uint32_t)rc;
    }

    return true;
}

static uint32_t am_align4(uint32_t value) {
    return (value + 3U) & ~3U;
}

static bool am_validate_metadata(const boredos_app_metadata_t *metadata) {
    if (!metadata) return false;
    if (metadata->magic != BOREDOS_APP_METADATA_MAGIC) return false;
    if (metadata->version != BOREDOS_APP_METADATA_VERSION) return false;
    if (metadata->image_count > BOREDOS_APP_METADATA_MAX_IMAGES) return false;
    return true;
}

static bool am_note_name_matches(const char *name, uint32_t name_size) {
    size_t expected_len = am_strlen(BOREDOS_APP_NOTE_OWNER);
    if (!name || name_size == 0) return false;
    if ((size_t)name_size < expected_len) return false;

    for (size_t i = 0; i < expected_len; i++) {
        if (name[i] != BOREDOS_APP_NOTE_OWNER[i]) return false;
    }
    return true;
}

static void am_sanitize_metadata(boredos_app_metadata_t *metadata) {
    if (!metadata) return;

    metadata->app_name[BOREDOS_APP_METADATA_MAX_APP_NAME - 1] = '\0';
    metadata->description[BOREDOS_APP_METADATA_MAX_DESCRIPTION - 1] = '\0';

    for (uint32_t i = 0; i < BOREDOS_APP_METADATA_MAX_IMAGES; i++) {
        metadata->images[i][BOREDOS_APP_METADATA_MAX_IMAGE_PATH - 1] = '\0';
    }

    if (metadata->image_count > BOREDOS_APP_METADATA_MAX_IMAGES) {
        metadata->image_count = BOREDOS_APP_METADATA_MAX_IMAGES;
    }
}

static bool am_parse_note_section(vfs_file_t *file,
                                  const Elf64_Shdr *section,
                                  boredos_app_metadata_t *out_metadata) {
    uint32_t offset = 0;

    if (!file || !section || !out_metadata) return false;

    while ((uint64_t)offset + sizeof(Elf64_Nhdr) <= section->sh_size) {
        Elf64_Nhdr nhdr;
        if (!am_seek(file, section->sh_offset + offset)) return false;
        if (!am_read_exact(file, &nhdr, sizeof(Elf64_Nhdr))) return false;

        offset += (uint32_t)sizeof(Elf64_Nhdr);

        if ((uint64_t)offset + nhdr.n_namesz > section->sh_size) return false;
        if (nhdr.n_namesz > 256U) return false;

        char *name_buf = (char *)kmalloc((size_t)nhdr.n_namesz + 1U);
        if (!name_buf) return false;

        if (!am_seek(file, section->sh_offset + offset) || !am_read_exact(file, name_buf, nhdr.n_namesz)) {
            kfree(name_buf);
            return false;
        }
        name_buf[nhdr.n_namesz] = '\0';

        offset += nhdr.n_namesz;
        offset = am_align4(offset);

        if ((uint64_t)offset + nhdr.n_descsz > section->sh_size) {
            kfree(name_buf);
            return false;
        }

        bool is_target_note = (nhdr.n_type == BOREDOS_APP_NOTE_TYPE) && am_note_name_matches(name_buf, nhdr.n_namesz);
        kfree(name_buf);

        if (is_target_note) {
            if (nhdr.n_descsz < sizeof(boredos_app_metadata_t)) return false;

            boredos_app_metadata_t metadata;
            if (!am_seek(file, section->sh_offset + offset) ||
                !am_read_exact(file, &metadata, (uint32_t)sizeof(boredos_app_metadata_t))) {
                return false;
            }

            if (!am_validate_metadata(&metadata)) return false;

            am_sanitize_metadata(&metadata);
            *out_metadata = metadata;
            return true;
        }

        offset += nhdr.n_descsz;
        offset = am_align4(offset);
    }

    return false;
}

static bool am_scan_raw_notes(vfs_file_t *file, uint32_t file_size, boredos_app_metadata_t *out_metadata) {
    uint8_t *buf = NULL;
    size_t owner_len = am_strlen(BOREDOS_APP_NOTE_OWNER);

    if (!file || !out_metadata || file_size < sizeof(Elf64_Nhdr) + owner_len + sizeof(boredos_app_metadata_t)) {
        return false;
    }
    if (file_size > 16U * 1024U * 1024U) {
        return false;
    }

    buf = (uint8_t *)kmalloc(file_size);
    if (!buf) return false;

    if (!am_seek(file, 0) || !am_read_exact(file, buf, file_size)) {
        kfree(buf);
        return false;
    }

    for (uint32_t off = 0; off + sizeof(Elf64_Nhdr) <= file_size; off++) {
        Elf64_Nhdr nhdr;
        uint32_t name_off;
        uint32_t desc_off;
        boredos_app_metadata_t metadata;

        am_mem_copy((uint8_t *)&nhdr, buf + off, sizeof(Elf64_Nhdr));
        if (nhdr.n_type != BOREDOS_APP_NOTE_TYPE) continue;
        if (nhdr.n_namesz < owner_len) continue;
        if (nhdr.n_descsz < sizeof(boredos_app_metadata_t)) continue;

        name_off = off + (uint32_t)sizeof(Elf64_Nhdr);
        if ((uint64_t)name_off + nhdr.n_namesz > file_size) continue;

        if (!am_mem_eq(buf + name_off, (const uint8_t *)BOREDOS_APP_NOTE_OWNER, owner_len)) continue;

        desc_off = name_off + am_align4(nhdr.n_namesz);
        if ((uint64_t)desc_off + sizeof(boredos_app_metadata_t) > file_size) continue;

        am_mem_copy((uint8_t *)&metadata, buf + desc_off, sizeof(boredos_app_metadata_t));
        if (!am_validate_metadata(&metadata)) continue;

        am_sanitize_metadata(&metadata);
        *out_metadata = metadata;
        kfree(buf);
        return true;
    }

    kfree(buf);
    return false;
}

static bool app_metadata_read_uncached(const char *path, boredos_app_metadata_t *out_metadata) {
    bool found = false;
    vfs_file_t *file = NULL;
    char *shstrtab = NULL;
    uint32_t file_size = 0;

    if (!path || !out_metadata) return false;

    file = vfs_open(path, "r");
    if (!file || !file->valid) goto cleanup;

    file_size = vfs_file_size(file);
    if (file_size > 0) {
        found = am_scan_raw_notes(file, file_size, out_metadata);
        if (found) goto cleanup;
    }

    Elf64_Ehdr ehdr;
    if (!am_read_exact(file, &ehdr, sizeof(Elf64_Ehdr))) goto cleanup;

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        goto cleanup;
    }

    if (ehdr.e_ident[4] != ELFCLASS64 || ehdr.e_ident[5] != ELFDATA2LSB) goto cleanup;
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shentsize < sizeof(Elf64_Shdr)) goto cleanup;
    if (ehdr.e_shstrndx == 0 || ehdr.e_shstrndx >= ehdr.e_shnum) goto cleanup;

    Elf64_Shdr shstr_hdr;
    uint64_t shstr_off = ehdr.e_shoff + ((uint64_t)ehdr.e_shstrndx * ehdr.e_shentsize);
    if (!am_seek(file, shstr_off) || !am_read_exact(file, &shstr_hdr, sizeof(Elf64_Shdr))) goto cleanup;
    if (shstr_hdr.sh_size == 0 || shstr_hdr.sh_size > 65536U) goto cleanup;

    shstrtab = (char *)kmalloc((size_t)shstr_hdr.sh_size + 1U);
    if (!shstrtab) goto cleanup;
    if (!am_seek(file, shstr_hdr.sh_offset) || !am_read_exact(file, shstrtab, (uint32_t)shstr_hdr.sh_size)) goto cleanup;
    shstrtab[shstr_hdr.sh_size] = '\0';

    for (uint16_t i = 0; i < ehdr.e_shnum; i++) {
        Elf64_Shdr shdr;
        uint64_t shdr_off = ehdr.e_shoff + ((uint64_t)i * ehdr.e_shentsize);
        if (!am_seek(file, shdr_off) || !am_read_exact(file, &shdr, sizeof(Elf64_Shdr))) goto cleanup;
        if (shdr.sh_type != SHT_NOTE) continue;
        if (shdr.sh_name >= shstr_hdr.sh_size) continue;

        const char *section_name = shstrtab + shdr.sh_name;
        if (!am_str_eq(section_name, BOREDOS_APP_NOTE_SECTION)) continue;

        if (am_parse_note_section(file, &shdr, out_metadata)) {
            found = true;
            break;
        }
    }

    if (!found && file_size > 0) {
        found = am_scan_raw_notes(file, file_size, out_metadata);
    }

cleanup:
    if (shstrtab) kfree(shstrtab);
    if (file) vfs_close(file);
    return found;
}

static bool app_metadata_cache_lookup(const char *path, boredos_app_metadata_t *out_metadata, bool *out_found) {
    if (!path || !out_found) return false;

    for (int i = 0; i < APP_METADATA_CACHE_SIZE; i++) {
        app_metadata_cache_entry_t *entry = &g_app_metadata_cache[i];
        if (!entry->valid) continue;
        if (!am_str_eq(entry->path, path)) continue;

        *out_found = entry->has_metadata;
        if (entry->has_metadata && out_metadata) {
            *out_metadata = entry->metadata;
        }
        return true;
    }

    return false;
}

static void app_metadata_cache_store(const char *path, const boredos_app_metadata_t *metadata, bool has_metadata) {
    app_metadata_cache_entry_t *entry;

    if (!path) return;

    entry = &g_app_metadata_cache[g_app_metadata_cache_next];
    g_app_metadata_cache_next = (g_app_metadata_cache_next + 1) % APP_METADATA_CACHE_SIZE;

    entry->valid = true;
    entry->has_metadata = has_metadata;
    am_str_copy(entry->path, path, sizeof(entry->path));
    if (has_metadata && metadata) {
        entry->metadata = *metadata;
    }
}

bool app_metadata_read(const char *path, boredos_app_metadata_t *out_metadata) {
    bool found = false;

    if (!path || !out_metadata) return false;

    if (app_metadata_cache_lookup(path, out_metadata, &found)) {
        return found;
    }

    found = app_metadata_read_uncached(path, out_metadata);
    app_metadata_cache_store(path, out_metadata, found);
    return found;
}

bool app_metadata_get_primary_image(const char *path, char *out_path, size_t out_path_size) {
    boredos_app_metadata_t metadata;

    if (!path || !out_path || out_path_size == 0) return false;
    out_path[0] = '\0';

    if (!app_metadata_read(path, &metadata)) return false;
    if (metadata.image_count == 0) return false;
    if (metadata.images[0][0] == '\0') return false;

    am_str_copy(out_path, metadata.images[0], out_path_size);
    return out_path[0] != '\0';
}
