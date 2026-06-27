// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "vfs.h"
#include "shm.h"
#include "memory_manager.h"
#include "spinlock.h"
#include <stddef.h>
#include "disk.h"
#include "process.h"
#include "tty.h"
#include "kutils.h"
#include "graphics.h"
#include "pcsk.h"
#include "ac97.h"
typedef framebuffer_info_t vfs_framebuffer_info_t;



static vfs_mount_t mounts[VFS_MAX_MOUNTS];
static int mount_count = 0;
static vfs_file_t open_files[VFS_MAX_OPEN_FILES];
static spinlock_t vfs_lock = SPINLOCK_INIT;

extern void serial_write(const char *str);
extern void serial_write_num(uint64_t num);
extern void serial_write_hex(uint64_t value);

static int vfs_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void vfs_strcpy(char *d, const char *s) {
    while ((*d++ = *s++));
}

static int vfs_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int vfs_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static bool vfs_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str++ != *prefix++) return false;
    }
    return true;
}

static bool vfs_path_is_parent(const char *parent, const char *child) {
    int plen = vfs_strlen(parent);
    if (vfs_strncmp(parent, child, plen) != 0) return false;
    if (child[plen] == '\0') return true;
    if (child[plen] == '/') return true;
    if (plen == 1 && parent[0] == '/') return true;
    return false;
}

void vfs_normalize_path(const char *cwd, const char *path, char *normalized) {
    char parts[32][64]; // Reduced size to save stack, 64 is enough for most names
    int depth = 0;
    int i = 0;

    // Handle relative path by starting with CWD
    if (path[0] != '/' && cwd) {
        int ci = 0;
        if (cwd[0] == '/') ci = 1;
        while (cwd[ci]) {
            if (cwd[ci] == '/') { ci++; continue; }
            int j = 0;
            while (cwd[ci] && cwd[ci] != '/' && j < 63) {
                parts[depth][j++] = cwd[ci++];
            }
            parts[depth][j] = 0;
            if (j > 0) depth++;
            if (depth >= 32) break;
            if (cwd[ci] == '/') ci++;
        }
    }

    if (path[0] == '/') i = 1;

    while (path[i]) {
        if (path[i] == '/') { i++; continue; }

        int j = 0;
        while (path[i] && path[i] != '/' && j < 63) {
            parts[depth][j++] = path[i++];
        }
        parts[depth][j] = 0;

        if (parts[depth][0] == '.' && parts[depth][1] == 0) {
            // "." skip
        } else if (parts[depth][0] == '.' && parts[depth][1] == '.' && parts[depth][2] == 0) {
            // ".." pop
            if (depth > 0) depth--;
        } else {
            if (j > 0) {
                depth++;
                if (depth >= 32) break;
            }
        }

        if (path[i] == '/') i++;
    }

    normalized[0] = '/';
    int pos = 1;
    for (int k = 0; k < depth; k++) {
        int l = 0;
        while (parts[k][l] && pos < VFS_MAX_PATH - 2) {
            normalized[pos++] = parts[k][l++];
        }
        if (k < depth - 1) normalized[pos++] = '/';
    }
    normalized[pos] = 0;

    if (pos == 1 && normalized[0] == '/') {
        normalized[1] = 0;
    }
}

static void vfs_normalize_process_path(const char *path, char *normalized) {
    process_t *proc = process_get_current();
    vfs_normalize_path(proc ? proc->cwd : "/", path, normalized);
}

static vfs_mount_t* vfs_resolve_mount(const char *path, const char **rel_path_out) {
    vfs_mount_t *best = NULL;
    int best_len = -1;

    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;

        int mlen = mounts[i].path_len;

        if (mlen == 1 && mounts[i].path[0] == '/') {
            if (best_len < 1) {
                best = &mounts[i];
                best_len = 1;
            }
            continue;
        }

        if (vfs_strncmp(path, mounts[i].path, mlen) == 0) {
            if (path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) {
                    best = &mounts[i];
                    best_len = mlen;
                }
            }
        }
    }

    if (best && rel_path_out) {
        const char *rel = path + best_len;
        while (*rel == '/') rel++;
        *rel_path_out = rel;
    }

    return best;
}

static vfs_file_t* vfs_alloc_file(void) {
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!open_files[i].valid) {
            open_files[i].valid = true;
            open_files[i].fs_handle = NULL;
            open_files[i].mount = NULL;
            open_files[i].position = 0;
            open_files[i].is_device = false;
            open_files[i].device_type = DEVICE_TYPE_BLOCK; // Initialize to safe default
            return &open_files[i];
        }
    }
    return NULL;
}

static void vfs_free_file(vfs_file_t *f) {
    if (f) {
        f->valid = false;
        f->fs_handle = NULL;
        f->mount = NULL;
        f->position = 0;
        f->is_device = false;
        f->device_type = DEVICE_TYPE_BLOCK;
    }
}

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].active = false;
    }
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        open_files[i].valid = false;
    }
    mount_count = 0;

    serial_write("[VFS] Ready\n");
}

// ===============
// Mount / Unmount
// ===============

bool vfs_mount(const char *mount_path, const char *device, const char *fs_type,
               vfs_fs_ops_t *ops, void *fs_private) {
    uint64_t flags = spinlock_acquire_irqsave(&vfs_lock);

    if (mount_count >= VFS_MAX_MOUNTS) {
        spinlock_release_irqrestore(&vfs_lock, flags);
        serial_write("[VFS] ERROR: Mount table full\n");
        return false;
    }

    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && vfs_strcmp(mounts[i].path, mount_path) == 0) {
            spinlock_release_irqrestore(&vfs_lock, flags);
            serial_write("[VFS] ERROR: Mount point already in use: ");
            serial_write(mount_path);
            serial_write("\n");
            return false;
        }
    }

    vfs_mount_t *m = &mounts[mount_count];
    vfs_strcpy(m->path, mount_path);
    m->path_len = vfs_strlen(mount_path);
    m->ops = ops;
    m->fs_private = fs_private;
    vfs_strcpy(m->device, device ? device : "none");
    vfs_strcpy(m->fs_type, fs_type ? fs_type : "unknown");
    m->active = true;
    mount_count++;

    spinlock_release_irqrestore(&vfs_lock, flags);

    serial_write("[VFS] Mounted ");
    serial_write(fs_type);
    serial_write(" (");
    serial_write(device ? device : "none");
    serial_write(") at ");
    serial_write(mount_path);
    serial_write("\n");

    return true;
}

bool vfs_umount(const char *mount_path) {
    uint64_t flags = spinlock_acquire_irqsave(&vfs_lock);

    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && vfs_strcmp(mounts[i].path, mount_path) == 0) {
            for (int j = 0; j < VFS_MAX_OPEN_FILES; j++) {
                if (open_files[j].valid && open_files[j].mount == &mounts[i]) {
                    if (mounts[i].ops->close) {
                        mounts[i].ops->close(mounts[i].fs_private, open_files[j].fs_handle);
                    }
                    vfs_free_file(&open_files[j]);
                }
            }

            serial_write("[VFS] Unmounted ");
            serial_write(mounts[i].path);
            serial_write("\n");

            mounts[i].active = false;

            // Compact array
            for (int k = i; k < mount_count - 1; k++) {
                mounts[k] = mounts[k + 1];
            }
            mount_count--;

            spinlock_release_irqrestore(&vfs_lock, flags);
            return true;
        }
    }

    spinlock_release_irqrestore(&vfs_lock, flags);
    return false;
}

// ==============
// File Operations
// ==============

vfs_file_t* vfs_open(const char *path, const char *mode) {
    if (!path || !mode) return NULL;

    char normalized[VFS_MAX_PATH];
    process_t *proc = process_get_current();
    vfs_normalize_path(proc ? proc->cwd : "/", path, normalized);

    uint64_t flags = spinlock_acquire_irqsave(&vfs_lock);

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    
    // Fallback for block devices (/dev/sda etc)
    if (vfs_starts_with(normalized, "/dev/")) {
        const char *devname = normalized + 5;
        
        // Handle TTY devices: /dev/ttyX
        if (vfs_starts_with(devname, "tty")) {
            int id = atoi(devname + 3);
            if (id >= 1 && id <= TTY_COUNT) {
                vfs_file_t *vf = vfs_alloc_file();
                if (vf) {
                    vf->mount = &mounts[0];
                    vf->fs_handle = (void*)(uintptr_t)(id - 1);
                    vf->is_device = true;
                    vf->device_type = DEVICE_TYPE_TTY;
                    spinlock_release_irqrestore(&vfs_lock, flags);
                    return vf;
                }
            }
        }
        
        if (vfs_starts_with(devname, "pts/")) {
            int idx = atoi(devname + 4);
            extern bool pty_is_pty_id(int id);
            extern void* pty_get(int pty_id);
            int pty_id = 1024 + idx;
            void *p = pty_get(pty_id);
            if (p) {
                vfs_file_t *vf = vfs_alloc_file();
                if (vf) {
                    vf->mount = &mounts[0];
                    vf->fs_handle = (void*)(uintptr_t)pty_id;
                    vf->is_device = true;
                    vf->device_type = DEVICE_TYPE_TTY;
                    spinlock_release_irqrestore(&vfs_lock, flags);
                    return vf;
                }
            }
        }
        
        // Handle Keyboard devices: /dev/keyboard (active) or /dev/keyboardX
        if (vfs_starts_with(devname, "keyboard")) {
            int id = 0;
            if (vfs_strcmp(devname, "keyboard") == 0) {
                id = tty_get_active_id() + 1;
            } else {
                id = atoi(devname + 8);
            }

            if (id >= 1 && id <= TTY_COUNT) {
                vfs_file_t *vf = vfs_alloc_file();
                if (vf) {
                    vf->mount = &mounts[0];
                    vf->fs_handle = (void*)(uintptr_t)(id - 1);
                    vf->is_device = true;
                    vf->device_type = DEVICE_TYPE_KEYBOARD;
                    spinlock_release_irqrestore(&vfs_lock, flags);
                    return vf;
                }
            }
        }

        // Handle Mouse devices: /dev/mouse (active) or /dev/mouseX
        if (vfs_starts_with(devname, "mouse")) {
            int id = 0;
            if (vfs_strcmp(devname, "mouse") == 0) {
                id = tty_get_active_id() + 1;
            } else {
                id = atoi(devname + 5);
            }

            if (id >= 1 && id <= TTY_COUNT) {
                vfs_file_t *vf = vfs_alloc_file();
                if (vf) {
                    vf->mount = &mounts[0];
                    vf->fs_handle = (void*)(uintptr_t)(id - 1);
                    vf->is_device = true;
                    vf->device_type = DEVICE_TYPE_MOUSE;
                    spinlock_release_irqrestore(&vfs_lock, flags);
                    return vf;
                }
            }
        }

        // Handle Framebuffer devices: /dev/fb0 or /dev/fbX
        if (vfs_starts_with(devname, "fb")) {
            int id = 0;
            if (vfs_strcmp(devname, "fb0") == 0 || vfs_strcmp(devname, "fb") == 0) {
                id = 0;
            } else if (devname[2] >= '0' && devname[2] <= '9') {
                id = atoi(devname + 2);
            } else {
                id = -1;
            }

            if (id >= 0 && id <= 0) { // Currently only support /dev/fb0
                vfs_file_t *vf = vfs_alloc_file();
                if (vf) {
                    vf->mount = &mounts[0];
                    vf->fs_handle = (void*)(uintptr_t)id;
                    vf->is_device = true;
                    vf->device_type = DEVICE_TYPE_FRAMEBUFFER;
                    vf->position = 0;
                    spinlock_release_irqrestore(&vfs_lock, flags);
                    return vf;
                }
            }
        }

        // Handle PC speaker device: /dev/pcsk 
        if (vfs_strcmp(devname, "pcsk") == 0) {
            vfs_file_t *vf = vfs_alloc_file();
            if (vf) {
                vf->mount = &mounts[0];
                vf->fs_handle = (void*)0;
                vf->is_device = true;
                vf->device_type = DEVICE_TYPE_PCSPKR;
                vf->position = 0;
                spinlock_release_irqrestore(&vfs_lock, flags);
                return vf;
            }
        }

        // Handle DSP (audio) device: /dev/dsp
        if (vfs_strcmp(devname, "dsp") == 0) {
            if (!ac97_present()) {
                spinlock_release_irqrestore(&vfs_lock, flags);
                return NULL;
            }
            vfs_file_t *vf = vfs_alloc_file();
            if (vf) {
                vf->mount = &mounts[0];
                vf->fs_handle = ac97_open_client();
                vf->is_device = true;
                vf->device_type = DEVICE_TYPE_AUDIO;
                vf->position = 0;
                spinlock_release_irqrestore(&vfs_lock, flags);
                return vf;
            }
        }

        // Handle Mixer device: /dev/mixer
        if (vfs_strcmp(devname, "mixer") == 0) {
            if (!ac97_present()) {
                spinlock_release_irqrestore(&vfs_lock, flags);
                return NULL;
            }
            vfs_file_t *vf = vfs_alloc_file();
            if (vf) {
                vf->mount = &mounts[0];
                vf->fs_handle = (void*)0;
                vf->is_device = true;
                vf->device_type = DEVICE_TYPE_MIXER;
                vf->position = 0;
                spinlock_release_irqrestore(&vfs_lock, flags);
                return vf;
            }
        }

        // Handle Shared Memory: /dev/shm/some_name
        if (vfs_starts_with(devname, "shm/")) {
            const char *shm_name = devname + 4;
            if (shm_name[0] != '\0') {
                typedef struct shm_segment shm_segment_t;
                extern shm_segment_t* shm_get_or_create(const char *name);
                shm_segment_t *seg = shm_get_or_create(shm_name);
                if (seg) {
                    vfs_file_t *vf = vfs_alloc_file();
                    if (vf) {
                        vf->mount = &mounts[0];
                        vf->fs_handle = (void*)seg;
                        vf->is_device = true;
                        vf->device_type = DEVICE_TYPE_SHM;
                        vf->position = 0;
                        spinlock_release_irqrestore(&vfs_lock, flags);
                        return vf;
                    } else {
                        extern void shm_unref(shm_segment_t *seg);
                        shm_unref(seg);
                    }
                }
            }
        }

        Disk *d = disk_get_by_name(devname);

        if (d && (!mount || mount->path_len == 1)) {
            vfs_file_t *vf = vfs_alloc_file();
            if (vf) {
                vf->mount = &mounts[0];
                vf->fs_handle = (void*)d; 
                vf->is_device = true;
                vf->position = 0;
                spinlock_release_irqrestore(&vfs_lock, flags);
                return vf;
            }
        }
    }
    
    if (!mount || !mount->ops->open) {
        spinlock_release_irqrestore(&vfs_lock, flags);
        return NULL;
    }

    if (!rel_path || rel_path[0] == '\0') {
        rel_path = "/";
    }

    vfs_file_t *vf = vfs_alloc_file();
    if (!vf) {
        spinlock_release_irqrestore(&vfs_lock, flags);
        serial_write("[VFS] ERROR: No free file handles\n");
        return NULL;
    }

    vf->mount = mount;

    spinlock_release_irqrestore(&vfs_lock, flags);

    void *fs_handle = mount->ops->open(mount->fs_private, rel_path, mode);
    if (!fs_handle) {
        flags = spinlock_acquire_irqsave(&vfs_lock);
        vfs_free_file(vf);
        spinlock_release_irqrestore(&vfs_lock, flags);
        return NULL;
    }

    vf->fs_handle = fs_handle;
    return vf;
}

void vfs_close(vfs_file_t *file) {
    if (!file || !file->valid) return;

    if (file->is_device && file->device_type == DEVICE_TYPE_SHM) {
        typedef struct shm_segment shm_segment_t;
        extern void shm_unref(shm_segment_t *seg);
        shm_unref((shm_segment_t *)file->fs_handle);
    }

    if (file->is_device && file->device_type == DEVICE_TYPE_AUDIO) {
        ac97_close_client(file->fs_handle);
    }

    vfs_mount_t *mount = file->mount;
    if (mount && mount->ops->close && !file->is_device) {
        mount->ops->close(mount->fs_private, file->fs_handle);
    }

    uint64_t flags = spinlock_acquire_irqsave(&vfs_lock);
    vfs_free_file(file);
    spinlock_release_irqrestore(&vfs_lock, flags);
}

int vfs_read(vfs_file_t *file, void *buf, int size) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return -1;
    
    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_BLOCK) {
            Disk *d = (Disk*)file->fs_handle;
            if (!d) return -1;
            
            uint32_t total_read = 0;
            uint32_t sector = (uint32_t)(file->position / 512);
            uint32_t offset = (uint32_t)(file->position % 512);
            uint8_t sector_buf[512];
            
            while (total_read < (uint32_t)size) {
                if (sector >= d->total_sectors) break;
                if (d->read_sector(d, sector, sector_buf) != 0) break;
                
                uint32_t to_copy = 512 - offset;
                if (to_copy > (uint32_t)size - total_read) to_copy = (uint32_t)size - total_read;
                
                memcpy((uint8_t*)buf + total_read, sector_buf + offset, to_copy);
                
                total_read += to_copy;
                file->position += to_copy;
                sector++;
                offset = 0;
            }
            return (int)total_read;
        } else if (file->device_type == DEVICE_TYPE_TTY) {
            return tty_read_input((int)(uintptr_t)file->fs_handle, (char*)buf, (size_t)size);
        } else if (file->device_type == DEVICE_TYPE_KEYBOARD) {
            return tty_read_key((int)(uintptr_t)file->fs_handle, (uint8_t*)buf, size);
        } else if (file->device_type == DEVICE_TYPE_MOUSE) {
            return tty_read_mouse((int)(uintptr_t)file->fs_handle, (uint8_t*)buf, size);
        } else if (file->device_type == DEVICE_TYPE_SHM) {
            typedef struct shm_segment shm_segment_t;
            shm_segment_t *seg = (shm_segment_t *)file->fs_handle;
            if (!seg || seg->page_count == 0) return -1;
            if (file->position >= seg->size) return 0;
            
            uint64_t to_read = seg->size - file->position;
            if ((uint64_t)size < to_read) to_read = size;
            
            uint64_t read_accum = 0;
            while (read_accum < to_read) {
                uint64_t current_pos = file->position + read_accum;
                uint32_t page_idx = (uint32_t)(current_pos / 4096);
                uint32_t page_off = (uint32_t)(current_pos % 4096);
                
                uint64_t chunk = 4096 - page_off;
                if (chunk > to_read - read_accum) chunk = to_read - read_accum;
                
                extern uint64_t p2v(uint64_t phys);
                void *page_vaddr = (void *)p2v(seg->phys_pages[page_idx]);
                memcpy((uint8_t*)buf + read_accum, (uint8_t*)page_vaddr + page_off, chunk);
                
                read_accum += chunk;
            }
            file->position += to_read;
            return (int)to_read;
        } else if (file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
            // Read framebuffer data (raw pixel data)
            vfs_framebuffer_info_t fb = graphics_get_fb_backing_params();
            
            if (!fb.address || fb.width == 0 || fb.height == 0) return -1;
            
            uint64_t fb_size = (uint64_t)fb.width * fb.height * (fb.bpp / 8);
            if (file->position >= fb_size) return 0;
            
            uint64_t to_read = fb_size - file->position;
            if ((uint64_t)size < to_read) to_read = size;
            
            memcpy(buf, (uint8_t*)fb.address + file->position, to_read);
            file->position += to_read;
            return (int)to_read;
        }
        else if (file->device_type == DEVICE_TYPE_AUDIO) {
            return ac97_read(file->fs_handle, buf, size);
        }
        return -1;
    }


    if (!file->mount->ops->read) return -1;
    int ret = file->mount->ops->read(file->mount->fs_private, file->fs_handle, buf, size);
    if (ret > 0) file->position += ret;
    return ret;
}

int vfs_write(vfs_file_t *file, const void *buf, int size) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return -1;

    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_TTY) {
            tty_write((int)(uintptr_t)file->fs_handle, (const char*)buf, size);
            return size;
        } else if (file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
            vfs_framebuffer_info_t fb = graphics_get_fb_backing_params();
            
            if (!fb.address || fb.width == 0 || fb.height == 0) return -1;
            
            uint64_t fb_size = (uint64_t)fb.width * fb.height * (fb.bpp / 8);
            if (file->position >= fb_size) return -1;
            
            uint64_t to_write = fb_size - file->position;
            if ((uint64_t)size < to_write) to_write = size;
            
            memcpy((uint8_t*)fb.address + file->position, buf, to_write);
            uint64_t start_pos = file->position;
            uint64_t end_pos = file->position + to_write - 1;
            int bytes_per_pixel = fb.bpp / 8;
            int start_row = (int)(start_pos / fb.pitch);
            int end_row = (int)(end_pos / fb.pitch);
            if (start_row < 0) start_row = 0;
            if (end_row >= (int)fb.height) end_row = fb.height - 1;
            int dirty_h = end_row - start_row + 1;
            if (dirty_h > 0) {
                graphics_mark_dirty(0, start_row, fb.width, dirty_h);
            }

            file->position += to_write;
            return (int)to_write;
        } else if (file->device_type == DEVICE_TYPE_SHM) {
            typedef struct shm_segment shm_segment_t;
            extern int shm_allocate(shm_segment_t *seg, size_t size);
            shm_segment_t *seg = (shm_segment_t *)file->fs_handle;
            if (!seg) return -1;

            // Grow the segment if this write extends beyond current size
            size_t end_pos = (size_t)file->position + (size_t)size;
            if (end_pos > seg->size) {
                if (shm_allocate(seg, end_pos) < 0) return -1;
            }
            if (file->position >= seg->size) return -1;
            
            uint64_t to_write = seg->size - file->position;
            if ((uint64_t)size < to_write) to_write = size;
            
            uint64_t write_accum = 0;
            while (write_accum < to_write) {
                uint64_t current_pos = file->position + write_accum;
                uint32_t page_idx = (uint32_t)(current_pos / 4096);
                uint32_t page_off = (uint32_t)(current_pos % 4096);
                
                uint64_t chunk = 4096 - page_off;
                if (chunk > to_write - write_accum) chunk = to_write - write_accum;
                
                extern uint64_t p2v(uint64_t phys);
                void *page_vaddr = (void *)p2v(seg->phys_pages[page_idx]);
                memcpy((uint8_t*)page_vaddr + page_off, (const uint8_t*)buf + write_accum, chunk);
                
                write_accum += chunk;
            }
            file->position += to_write;
            return (int)to_write;
        }
        else if (file->device_type == DEVICE_TYPE_AUDIO) {
            return ac97_write(file->fs_handle, buf, size);
        }
        return -1;
    }

    if (!file->mount->ops->write) return -1;


    return file->mount->ops->write(file->mount->fs_private, file->fs_handle, buf, size);
}
int vfs_ioctl(vfs_file_t *file, uint64_t request, void *arg) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return -1;
    
    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_TTY) {
            extern int tty_ioctl(int id, uint64_t request, void *arg);
            return tty_ioctl((int)(uintptr_t)file->fs_handle, request, arg);
        } else if (file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
            // Handle framebuffer ioctls
            
            // Linux framebuffer ioctl commands
            #define FBIOGET_VSCREENINFO 0x4600
            #define FBIOPUT_VSCREENINFO 0x4601
            #define FBIOGET_FSCREENINFO 0x4602
            #define FBIOPAN_DISPLAY     0x4604
            #define FBIOGETCMAP         0x4604
            #define FBIOPUTCMAP         0x4605
            #define FBIOSET_DIRTY       0x4606
            
            vfs_framebuffer_info_t fb = graphics_get_fb_backing_params();
           /* 
            serial_write("[vfs_ioctl] Framebuffer request=");
            serial_write_hex(request);
            serial_write(" arg=");
            serial_write_hex((uint64_t)arg);
            serial_write(" fb_addr=");
            serial_write_hex((uint64_t)fb.address);
            serial_write(" fb_w=");
            serial_write_num(fb.width);
            serial_write(" fb_h=");
            serial_write_num(fb.height);
            serial_write(" fb_bpp=");
            serial_write_num(fb.bpp);
            serial_write("\n"); */
            
            // Validate framebuffer is initialized
            if (!fb.address || fb.width == 0 || fb.height == 0 || fb.bpp == 0) {
                serial_write("[vfs_ioctl] Validation failed! fb.address=");
                serial_write_hex((uint64_t)fb.address);
                serial_write(" fb.width=");
                serial_write_num(fb.width);
                serial_write(" fb.height=");
                serial_write_num(fb.height);
                serial_write(" fb.bpp=");
                serial_write_num(fb.bpp);
                serial_write("\n");
                return -1;
            }
            
            if (request == FBIOGET_VSCREENINFO) {
                // Return video screen info (variable info)
                if (!arg) return -1;
                
                typedef struct {
                    uint32_t xres;
                    uint32_t yres;
                    uint32_t xres_virtual;
                    uint32_t yres_virtual;
                    uint32_t xoffset;
                    uint32_t yoffset;
                    uint32_t bits_per_pixel;
                    uint32_t grayscale;
                    struct { uint32_t offset; uint32_t length; } red, green, blue, transp;
                    uint32_t nonstd;
                    uint32_t activate;
                    uint32_t height;
                    uint32_t width;
                    uint32_t accel_flags;
                    uint32_t pixclock;
                    uint32_t left_margin;
                    uint32_t right_margin;
                    uint32_t upper_margin;
                    uint32_t lower_margin;
                    uint32_t hsync_len;
                    uint32_t vsync_len;
                    uint32_t sync;
                    uint32_t vmode;
                    uint32_t rotate;
                    uint32_t colorspace;
                    uint32_t reserved[4];
                } fb_var_screeninfo_t;
                
                fb_var_screeninfo_t *vinfo = (fb_var_screeninfo_t *)arg;
                vinfo->xres = fb.width;
                vinfo->yres = fb.height;
                vinfo->xres_virtual = fb.width;
                vinfo->yres_virtual = fb.height;
                vinfo->xoffset = 0;
                vinfo->yoffset = 0;
                vinfo->bits_per_pixel = fb.bpp;
                vinfo->grayscale = 0;
                vinfo->red.offset = fb.red_mask_shift;
                vinfo->red.length = fb.red_mask_size;
                vinfo->green.offset = fb.green_mask_shift;
                vinfo->green.length = fb.green_mask_size;
                vinfo->blue.offset = fb.blue_mask_shift;
                vinfo->blue.length = fb.blue_mask_size;
                vinfo->transp.offset = 0;
                vinfo->transp.length = 0;
                vinfo->nonstd = 0;
                vinfo->activate = 0;
                vinfo->height = 0;
                vinfo->width = 0;
                vinfo->accel_flags = 0;
                vinfo->pixclock = 0;
                vinfo->left_margin = 0;
                vinfo->right_margin = 0;
                vinfo->upper_margin = 0;
                vinfo->lower_margin = 0;
                vinfo->hsync_len = 0;
                vinfo->vsync_len = 0;
                vinfo->sync = 0;
                vinfo->vmode = 0;
                vinfo->rotate = 0;
                vinfo->colorspace = 0;
                
                return 0;
            } else if (request == FBIOGET_FSCREENINFO) {
                // Return fixed screen info
                if (!arg) return -1;
                
                typedef struct {
                    char id[16];
                    uint64_t smem_start;
                    uint32_t smem_len;
                    uint32_t type;
                    uint32_t type_aux;
                    uint32_t visual;
                    uint16_t xpanstep;
                    uint16_t ypanstep;
                    uint16_t ywrapstep;
                    uint32_t line_length;
                    uint64_t mmio_start;
                    uint32_t mmio_len;
                    uint32_t accel;
                    uint16_t reserved[3];
                } fb_fix_screeninfo_t;
                
                fb_fix_screeninfo_t *finfo = (fb_fix_screeninfo_t *)arg;
                vfs_strcpy(finfo->id, "BoredOS FB");
                finfo->smem_start = (uint64_t)fb.address;
                finfo->smem_len = (uint32_t)(fb.width * fb.height * (fb.bpp / 8));
                finfo->type = 0; // FB_TYPE_PACKED_PIXELS
                finfo->type_aux = 0;
                finfo->visual = 2; // FB_VISUAL_TRUECOLOR
                finfo->xpanstep = 0;
                finfo->ypanstep = 0;
                finfo->ywrapstep = 0;
                finfo->line_length = (uint32_t)fb.pitch;
                finfo->mmio_start = 0;
                finfo->mmio_len = 0;
                finfo->accel = 0;
                
                return 0;
            } else if (request == FBIOPAN_DISPLAY) {
                graphics_present_framebuffer();
                return 0;
            } else if (request == FBIOSET_DIRTY) {
                if (!arg) return -1;
                struct { int x; int y; int w; int h; } *r = (void *)arg;
                graphics_mark_dirty(r->x, r->y, r->w, r->h);
                return 0;
            } else if (request == FBIOPUT_VSCREENINFO) {
                // Ignore changes as our FB is fixed by the bootloader, but report success
                return 0;
            }
            return -1;
        }
        else if (file->device_type == DEVICE_TYPE_PCSPKR) {
            extern int pcsk_ioctl(void *file_handle, uint64_t request, void *arg);
            return pcsk_ioctl(file->fs_handle, request, arg);
        }
        else if (file->device_type == DEVICE_TYPE_AUDIO) {
            return ac97_dsp_ioctl(file->fs_handle, request, arg);
        }
        else if (file->device_type == DEVICE_TYPE_MIXER) {
            return ac97_mixer_ioctl(request, arg);
        }
        return -1;
    }
    
    if (file->mount->ops->ioctl) {
        return file->mount->ops->ioctl(file->mount->fs_private, file->fs_handle, request, arg);
    }
    
    return -1;
}

int vfs_seek(vfs_file_t *file, int offset, int whence) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return -1;
    
    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
            // Seek in framebuffer
            vfs_framebuffer_info_t fb = graphics_get_fb_backing_params();
            
            if (!fb.address || fb.width == 0 || fb.height == 0) return -1;
            
            uint64_t fb_size = (uint64_t)fb.width * fb.height * (fb.bpp / 8);
            uint64_t new_pos = file->position;
            
            if (whence == 0) new_pos = (uint64_t)offset; // SEEK_SET
            else if (whence == 1) new_pos += (uint64_t)offset; // SEEK_CUR
            else if (whence == 2) new_pos = fb_size + (uint64_t)offset; // SEEK_END
            else return -1;
            
            if (new_pos > fb_size) new_pos = fb_size;
            file->position = new_pos;
            return 0;
        } else if (file->device_type == DEVICE_TYPE_SHM) {
            typedef struct shm_segment shm_segment_t;
            shm_segment_t *seg = (shm_segment_t *)file->fs_handle;
            if (!seg) return -1;
            uint64_t new_pos = file->position;
            if (whence == 0) new_pos = (uint64_t)offset; // SEEK_SET
            else if (whence == 1) new_pos += (uint64_t)offset; // SEEK_CUR
            else if (whence == 2) new_pos = (uint64_t)seg->size + (uint64_t)offset; // SEEK_END
            else return -1;
            
            if (new_pos > seg->size) new_pos = seg->size;
            file->position = new_pos;
            return 0;
        } else if (file->device_type == DEVICE_TYPE_BLOCK) {
            Disk *d = (Disk*)file->fs_handle;
            if (!d) return -1;
            uint64_t new_pos = file->position;
            if (whence == 0) new_pos = (uint64_t)offset; // SET
            else if (whence == 1) new_pos += (uint64_t)offset; // CUR
            else if (whence == 2) new_pos = (uint64_t)(d->total_sectors * 512 + offset); // END
            
            if (new_pos > (uint64_t)d->total_sectors * 512) new_pos = (uint64_t)d->total_sectors * 512;
            file->position = new_pos;
            return 0;
        } else {
            // Seek not supported on other device types (TTY, Keyboard, Mouse)
            return -1;
        }
    }

    if (!file->mount->ops->seek) return -1;
    int ret = file->mount->ops->seek(file->mount->fs_private, file->fs_handle, offset, whence);
    if (ret == 0) {
        // Sync position back from driver if possible
        if (file->mount->ops->get_position) {
            file->position = file->mount->ops->get_position(file->fs_handle);
        } else {
            // Manual sync if driver doesn't support get_position but seek succeeded
            if (whence == 0) file->position = offset;
            else if (whence == 1) file->position += offset;
        }
    }
    return ret;
}

int vfs_poll(vfs_file_t *file, struct poll_table *pt) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return POLLNVAL;
    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_TTY || file->device_type == DEVICE_TYPE_KEYBOARD || file->device_type == DEVICE_TYPE_MOUSE) {
            int handle_id = (int)(uintptr_t)file->fs_handle;
            extern bool pty_is_pty_id(int id);
            extern int pty_poll(int pty_id, struct poll_table *pt);
            if (file->device_type == DEVICE_TYPE_TTY && pty_is_pty_id(handle_id)) {
                return pty_poll(handle_id, pt);
            }
            tty_t *t = tty_get(handle_id);
            if (!t) return POLLNVAL;
            
            tty_queue_t *q = NULL;
            if (file->device_type == DEVICE_TYPE_TTY) q = &t->char_queue;
            else if (file->device_type == DEVICE_TYPE_KEYBOARD) q = &t->key_queue;
            else q = &t->mouse_queue;

            if (pt && pt->qproc) {
                pt->qproc(&q->wait_queue, pt);
            }

            int mask = 0;
            uint64_t flags = spinlock_acquire_irqsave(&t->lock);
            if (q->head != q->tail) mask |= POLLIN;
            mask |= POLLOUT;
            spinlock_release_irqrestore(&t->lock, flags);
            return mask;
        }
        return POLLIN | POLLOUT;
    }

    if (!file->mount->ops->poll) {
        return POLLIN | POLLOUT;
    }
    return file->mount->ops->poll(file->mount->fs_private, file->fs_handle, pt);
}

uint32_t vfs_file_position(vfs_file_t *file) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return 0;
    if (file->is_device) return (uint32_t)file->position;
    if (!file->mount->ops->get_position) return 0;
    return file->mount->ops->get_position(file->fs_handle);
}

uint32_t vfs_file_size(vfs_file_t *file) {
    if (!file || !file->valid || !file->mount || !file->mount->active) return 0;
    if (file->is_device) {
        if (file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
            vfs_framebuffer_info_t fb = graphics_get_fb_backing_params();
            return (uint32_t)(fb.width * fb.height * (fb.bpp / 8));
        }
        if (file->device_type == DEVICE_TYPE_SHM) {
            typedef struct shm_segment shm_segment_t;
            shm_segment_t *seg = (shm_segment_t *)file->fs_handle;
            return seg ? seg->size : 0;
        }
        Disk *d = (Disk*)file->fs_handle;
        return d ? d->total_sectors * 512 : 0;
    }
    if (!file->mount->ops->get_size) return 0;
    return file->mount->ops->get_size(file->fs_handle);
}



int vfs_list_directory(const char *path, vfs_dirent_t *entries, int max, int offset) {
    if (!path || !entries) return -1;
    

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    
    int count = 0;
    if (mount && mount->ops->readdir) {
        if (!rel_path || rel_path[0] == '\0') rel_path = "/";
        count = mount->ops->readdir(mount->fs_private, rel_path, entries, max, offset);
        if (count < 0) count = 0; 
    }

    if (offset == 0) {
        uint64_t v_flags = spinlock_acquire_irqsave(&vfs_lock);
        for (int i = 0; i < mount_count; i++) {
            if (!mounts[i].active) continue;
            if (vfs_strcmp(mounts[i].path, normalized) == 0) continue; 

            if (vfs_path_is_parent(normalized, mounts[i].path)) {
                const char *sub = mounts[i].path + vfs_strlen(normalized);
                if (*sub == '/') sub++; 

                if (*sub != '\0') {
                    char comp[VFS_MAX_NAME];
                    int j = 0;
                    while (sub[j] && sub[j] != '/' && j < VFS_MAX_NAME - 1) {
                        comp[j] = sub[j];
                        j++;
                    }
                    comp[j] = 0;

                    bool found = false;
                    for (int k = 0; k < count; k++) {
                        if (vfs_strcmp(entries[k].name, comp) == 0) {
                            found = true;
                            break;
                        }
                    }

                    if (!found && count < max) {
                        vfs_strcpy(entries[count].name, comp);
                        entries[count].is_directory = 1;
                        entries[count].size = 0;
                        entries[count].start_cluster = 0;
                        count++;
                    }
                }
            }
        }
        spinlock_release_irqrestore(&vfs_lock, v_flags);

        // Special case: Ensure "dev", "sys", "proc" are visible in "/"
        if (vfs_strcmp(normalized, "/") == 0) {
            const char *virtual_dirs[] = {"dev", "sys", "proc"};
            for (int v = 0; v < 3; v++) {
                bool found = false;
                for (int i = 0; i < count; i++) {
                    if (vfs_strcmp(entries[i].name, virtual_dirs[v]) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found && count < max) {
                    vfs_strcpy(entries[count].name, virtual_dirs[v]);
                    entries[count].is_directory = 1;
                    entries[count].size = 0;
                    entries[count].start_cluster = 0;
                    count++;
                }
            }
        }

        // Special case: /dev listing for block devices and TTYs
        if (vfs_strcmp(normalized, "/dev") == 0) {
            // TTY devices
            for (int i = 0; i < TTY_COUNT && count < max; i++) {
                char name[16];
                vfs_strcpy(name, "tty");
                int pos = 3;
                if (i + 1 >= 10) name[pos++] = '1';
                name[pos++] = '0' + ((i + 1) % 10);
                name[pos] = '\0';
                
                vfs_strcpy(entries[count].name, name);
                entries[count].size = 0;
                entries[count].is_directory = 0;
                count++;
            }

            // Input devices (singular aliases)
            if (count < max) {
                vfs_strcpy(entries[count].name, "keyboard");
                entries[count].size = 0;
                entries[count].is_directory = 0;
                count++;
            }
            if (count < max) {
                vfs_strcpy(entries[count].name, "mouse");
                entries[count].size = 0;
                entries[count].is_directory = 0;
                count++;
            }

            // PC speaker device
            if (count < max) {
                vfs_strcpy(entries[count].name, "pcsk");
                entries[count].size = 0;
                entries[count].is_directory = 0;
                count++;
            }

            // DSP (audio) device
            if (count < max) {
                if (ac97_present()) {
                    vfs_strcpy(entries[count].name, "dsp");
                    entries[count].size = 0;
                    entries[count].is_directory = 0;
                    count++;
                }
            }

            // Mixer device
            if (count < max) {
                if (ac97_present()) {
                    vfs_strcpy(entries[count].name, "mixer");
                    entries[count].size = 0;
                    entries[count].is_directory = 0;
                    count++;
                }
            }


            // Framebuffer device
            if (count < max) {
                vfs_strcpy(entries[count].name, "fb0");
                vfs_framebuffer_info_t fb = graphics_get_fb_params();
                entries[count].size = (uint64_t)fb.width * fb.height * (fb.bpp / 8);
                entries[count].is_directory = 0;
                count++;
            }

            // Shared memory directory
            if (count < max) {
                vfs_strcpy(entries[count].name, "shm");
                entries[count].size = 0;
                entries[count].is_directory = 1;
                count++;
            }

            int dcount = disk_get_count();
            for (int i = 0; i < dcount && count < max; i++) {
                Disk *d = disk_get_by_index(i);
                if (d && d->registered) {
                    // Ensure unique name (disk_manager_scan might register partitions)
                    bool found = false;
                    for (int k = 0; k < count; k++) {
                        if (vfs_strcmp(entries[k].name, d->devname) == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        vfs_strcpy(entries[count].name, d->devname);
                        entries[count].size = (uint64_t)d->total_sectors * 512;
                        entries[count].is_directory = 0;
                        entries[count].start_cluster = 0;
                        entries[count].write_date = 0;
                        entries[count].write_time = 0;
                        count++;
                    }
                }
            }
        }
    }

    return count;
}

bool vfs_mkdir(const char *path) {
    if (!path) return false;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);

    if (vfs_starts_with(normalized, "/dev/")) {
        if (!mount || !rel_path || rel_path[0] == '\0') {
            return false; 
        }
    }

    if (!mount || !mount->ops->mkdir) return false;
    return mount->ops->mkdir(mount->fs_private, rel_path);
}

bool vfs_rmdir(const char *path) {
    if (!path) return false;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    if (normalized[0] == '/' && normalized[1] == '\0') return false;
    if (vfs_strcmp(normalized, "/dev") == 0) return false;

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);

    if (vfs_starts_with(normalized, "/dev/")) {
        if (!mount || !rel_path || rel_path[0] == '\0') {
            return false; 
        }
    }

    if (!mount || !mount->ops->rmdir) return false;
    return mount->ops->rmdir(mount->fs_private, rel_path);
}

bool vfs_delete(const char *path) {
    if (!path) return false;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    if (normalized[0] == '/' && normalized[1] == '\0') return false;
    if (vfs_strcmp(normalized, "/dev") == 0) return false;

    if (vfs_starts_with(normalized, "/dev/shm/")) {
        const char *shm_name = normalized + 9;
        if (shm_name[0] != '\0') {
            extern void shm_unlink(const char *name);
            shm_unlink(shm_name);
            return true;
        }
    }

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);

    if (vfs_starts_with(normalized, "/dev/")) {
        if (!mount || !rel_path || rel_path[0] == '\0') {
            return false; 
        }
    }

    if (!mount || !mount->ops->unlink) return false;
    return mount->ops->unlink(mount->fs_private, rel_path);
}

bool vfs_rename(const char *old_path, const char *new_path) {
    if (!old_path || !new_path) return false;

    char norm_old[VFS_MAX_PATH], norm_new[VFS_MAX_PATH];
    vfs_normalize_process_path(old_path, norm_old);
    vfs_normalize_process_path(new_path, norm_new);

    const char *rel_old = NULL, *rel_new = NULL;
    vfs_mount_t *mount_old = vfs_resolve_mount(norm_old, &rel_old);
    vfs_mount_t *mount_new = vfs_resolve_mount(norm_new, &rel_new);

    if (!mount_old || mount_old != mount_new) return false;
    if (!mount_old->ops->rename) return false;

    if (!rel_old || rel_old[0] == '\0') return false;
    if (!rel_new || rel_new[0] == '\0') return false;

    return mount_old->ops->rename(mount_old->fs_private, rel_old, rel_new);
}

bool vfs_exists(const char *path) {
    if (!path) return false;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    if (normalized[0] == '/' && normalized[1] == '\0') return true;

    uint64_t flags_vfs = spinlock_acquire_irqsave(&vfs_lock);
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && vfs_starts_with(mounts[i].path, normalized)) {
            spinlock_release_irqrestore(&vfs_lock, flags_vfs);
            return true;
        }
    }
    spinlock_release_irqrestore(&vfs_lock, flags_vfs);

    if (vfs_strcmp(normalized, "/dev") == 0 || 
        vfs_strcmp(normalized, "/sys") == 0 || 
        vfs_strcmp(normalized, "/proc") == 0) return true;

    if (vfs_starts_with(normalized, "/dev/")) {
        const char *dev = normalized + 5;
        // Check for framebuffer device
        if (vfs_strcmp(dev, "fb0") == 0 || vfs_strcmp(dev, "fb") == 0) {
            vfs_framebuffer_info_t fb = graphics_get_fb_params();
            return fb.address != NULL && fb.width > 0 && fb.height > 0;
        }
        if (vfs_strcmp(dev, "keyboard") == 0 || vfs_starts_with(dev, "keyboard")) return true;
        if (vfs_strcmp(dev, "mouse") == 0 || vfs_starts_with(dev, "mouse")) return true;
        if (vfs_starts_with(dev, "tty")) return true;
        if (vfs_strcmp(dev, "pcsk") == 0) return true;
        if (vfs_strcmp(dev, "dsp") == 0) {
            return ac97_present();
        }
        if (vfs_strcmp(dev, "mixer") == 0) {
            return ac97_present();
        }
        if (vfs_strcmp(dev, "shm") == 0) return true;
        if (vfs_starts_with(dev, "shm/")) {
            extern bool shm_exists(const char *name);
            return shm_exists(dev + 4);
        }
        if (disk_get_by_name(dev)) return true;
    }

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    if (!mount || !mount->ops->exists) return false;

    if (!rel_path || rel_path[0] == '\0') return true; 

    return mount->ops->exists(mount->fs_private, rel_path);
}

bool vfs_is_directory(const char *path) {
    if (!path) return false;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    if (normalized[0] == '/' && normalized[1] == '\0') return true;

    uint64_t flags_vfs = spinlock_acquire_irqsave(&vfs_lock);
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && vfs_path_is_parent(normalized, mounts[i].path)) {
            if (vfs_strcmp(mounts[i].path, normalized) == 0) {
                spinlock_release_irqrestore(&vfs_lock, flags_vfs);
                return true;
            }
            // If normalized is a parent of a mount, it's a virtual directory
            spinlock_release_irqrestore(&vfs_lock, flags_vfs);
            return true;
        }
    }
    spinlock_release_irqrestore(&vfs_lock, flags_vfs);

    if (vfs_strcmp(normalized, "/dev") == 0 || 
        vfs_strcmp(normalized, "/dev/shm") == 0 || 
        vfs_strcmp(normalized, "/sys") == 0 || 
        vfs_strcmp(normalized, "/proc") == 0) return true;

    if (vfs_starts_with(normalized, "/dev/")) {
        const char *dev = normalized + 5;
        // Check if it's a framebuffer device (not a directory)
        if (vfs_strcmp(dev, "fb0") == 0 || vfs_strcmp(dev, "fb") == 0) return false;
        Disk *d = disk_get_by_name(dev);
        if (d) return false;
    }

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    if (!mount) return false;

    if (!rel_path || rel_path[0] == '\0') return true;

    if (!mount->ops->is_dir) return false;
    return mount->ops->is_dir(mount->fs_private, rel_path);
}

int vfs_statfs(const char *path, vfs_statfs_t *stat) {
    if (!path || !stat) return -1;
    
    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);
    
    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    if (!mount) return -1;
    
    if (mount->ops->statfs) {
        return mount->ops->statfs(mount->fs_private, stat);
    }
    
    stat->total_blocks = 0;
    stat->free_blocks = 0;
    stat->block_size = 512;
    return 0;
}

int vfs_get_info(const char *path, vfs_dirent_t *info) {
    if (!path || !info) return -1;

    char normalized[VFS_MAX_PATH];
    vfs_normalize_process_path(path, normalized);

    if (normalized[0] == '/' && normalized[1] == '\0') {
        vfs_strcpy(info->name, "/");
        info->size = 0;
        info->is_directory = 1;
        info->start_cluster = 0;
        info->write_date = 0;
        info->write_time = 0;
        return 0;
    }

    if (vfs_strcmp(normalized, "/dev") == 0 || 
        vfs_strcmp(normalized, "/sys") == 0 || 
        vfs_strcmp(normalized, "/proc") == 0) {
        const char *name = normalized + 1;
        vfs_strcpy(info->name, name);
        info->size = 0;
        info->is_directory = 1;
        info->start_cluster = 0;
        info->write_date = 0;
        info->write_time = 0;
        return 0;
    }

    uint64_t flags_vfs = spinlock_acquire_irqsave(&vfs_lock);
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && vfs_path_is_parent(normalized, mounts[i].path)) {
            if (vfs_strcmp(mounts[i].path, normalized) != 0) {
                const char *p = normalized + vfs_strlen(normalized);
                while (p > normalized && *(p-1) != '/') p--;
                vfs_strcpy(info->name, p);
                info->size = 0;
                info->is_directory = 1;
                info->start_cluster = 0;
                info->write_date = 0;
                info->write_time = 0;
                spinlock_release_irqrestore(&vfs_lock, flags_vfs);
                return 0;
            }
        }
    }
    spinlock_release_irqrestore(&vfs_lock, flags_vfs);

    // Device check
    if (vfs_starts_with(normalized, "/dev/")) {
        const char *dev = normalized + 5;
        Disk *d = disk_get_by_name(dev);
        if (d) {
            vfs_strcpy(info->name, d->devname);
            info->size = d->total_sectors * 512;
            info->is_directory = 0;
            info->start_cluster = 0;
            info->write_date = 0;
            info->write_time = 0;
            return 0;
        }
    }

    const char *rel_path = NULL;
    vfs_mount_t *mount = vfs_resolve_mount(normalized, &rel_path);
    if (!mount || !mount->ops->get_info) return -1;

    if (!rel_path || rel_path[0] == '\0') {
        // Info about mount root
        vfs_strcpy(info->name, mount->device);
        info->size = 0;
        info->is_directory = 1;
        info->start_cluster = 0;
        info->write_date = 0;
        info->write_time = 0;
        return 0;
    }

    return mount->ops->get_info(mount->fs_private, rel_path, info);
}

int vfs_get_mount_count(void) {
    return mount_count;
}

vfs_mount_t* vfs_get_mount(int index) {
    if (index < 0 || index >= mount_count) return NULL;
    if (!mounts[index].active) return NULL;
    return &mounts[index];
}

void vfs_automount_partition(const char *devname) {
    char mount_path[64] = "/mnt/";
    int i = 5;
    const char *d = devname;
    while (*d && i < 62) mount_path[i++] = *d++;
    mount_path[i] = 0;

    serial_write("[VFS] Auto-mount requested for ");
    serial_write(devname);
    serial_write(" at ");
    serial_write(mount_path);
    serial_write("\n");
}
