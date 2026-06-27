#include "vfs.h"
#include "process.h"
#include "syscall.h"
#include "disk.h"
#include "memory_manager.h"
#include "kutils.h"
#include "platform.h"
#include "version.h"

typedef struct {
    uint32_t pid;
    char type[32]; 
    int offset;
    bool is_root;
} procfs_handle_t;

void* procfs_open(void *fs_private, const char *path, const char *mode) {
    if (path[0] == '/') path++;
    
    procfs_handle_t *h = (procfs_handle_t*)kmalloc(sizeof(procfs_handle_t));
    memset(h, 0, sizeof(procfs_handle_t));
    h->offset = 0;

    if (path[0] == '\0') {
        h->is_root = true;
        return h;
    }

    if (path[0] >= '0' && path[0] <= '9') {
        char pid_str[16];
        int i = 0;
        while (path[i] && path[i] != '/' && i < 15) {
            pid_str[i] = path[i];
            i++;
        }
        pid_str[i] = 0;
        h->pid = atoi(pid_str);

        if (path[i] == '/') {
            strcpy(h->type, path + i + 1);
        } else {
            h->type[0] = 0; 
        }
        return h;
    }

    h->pid = 0xFFFFFFFF;
    strcpy(h->type, path);
    return h;
}

void procfs_close(void *fs_private, void *handle) {
    if (handle) kfree(handle);
}

int procfs_read(void *fs_private, void *handle, void *buf, int size) {
    procfs_handle_t *h = (procfs_handle_t*)handle;
    if (!h) return -1;

    char *out = (char*)kmalloc(16384);
    if (!out) return -1;
    out[0] = 0;

    if (h->pid == 0xFFFFFFFF) {
        if (strcmp(h->type, "version") == 0) {
            os_info_t info;
            get_os_info(&info);
            strcpy(out, info.os_name);
            strcpy(out + strlen(out), " [");
            strcpy(out + strlen(out), info.os_codename);
            strcpy(out + strlen(out), "] Version ");
            strcpy(out + strlen(out), info.os_version);
            strcpy(out + strlen(out), "\nKernel: ");
            strcpy(out + strlen(out), info.kernel_name);
            strcpy(out + strlen(out), " ");
            strcpy(out + strlen(out), info.kernel_version);
            strcpy(out + strlen(out), "\nBuild: ");
            strcpy(out + strlen(out), info.build_date);
            strcpy(out + strlen(out), " ");
            strcpy(out + strlen(out), info.build_time);
            strcpy(out + strlen(out), "\n");
        } else if (strcmp(h->type, "uptime") == 0) {
            extern uint32_t get_ticks(void);
            uint32_t ticks = get_ticks();
            itoa(ticks / 60, out);
            strcpy(out + strlen(out), " seconds\nRaw_Ticks:");
            char t_s[16]; itoa(ticks, t_s);
            strcpy(out + strlen(out), t_s);
            strcpy(out + strlen(out), "\n");
        } else if (strcmp(h->type, "cpuinfo") == 0) {
            extern uint32_t smp_cpu_count(void);
            extern void platform_get_cpu_model(char *model);
            extern void platform_get_cpu_vendor(char *vendor);
            extern void platform_get_cpu_info(cpu_info_t *info);
            extern void platform_get_cpu_flags(char *flags_str);
            
            char model[64];
            char vendor[16];
            char flags[1024];
            cpu_info_t info;
            
            platform_get_cpu_model(model);
            platform_get_cpu_vendor(vendor);
            platform_get_cpu_info(&info);
            platform_get_cpu_flags(flags);
            
            uint32_t cpu_count = smp_cpu_count();
            out[0] = '\0';
            
            // Output info for each processor
            for (uint32_t i = 0; i < cpu_count; i++) {
                char buf[32];
                
                strcpy(out + strlen(out), "processor\t: ");
                itoa(i, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "vendor_id\t: ");
                strcpy(out + strlen(out), vendor);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "cpu family\t: ");
                itoa(info.family, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "model\t\t: ");
                itoa(info.model, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "model name\t: ");
                strcpy(out + strlen(out), model);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "stepping\t: ");
                itoa(info.stepping, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "microcode\t: 0x");
                char hex[16];
                int temp = info.microcode;
                int hex_pos = 0;
                for (int j = 7; j >= 0; j--) {
                    int digit = (temp >> (j * 4)) & 0xF;
                    hex[hex_pos++] = digit < 10 ? '0' + digit : 'a' + (digit - 10);
                }
                hex[hex_pos] = '\0';
                strcpy(out + strlen(out), hex);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "cache size\t: ");
                itoa(info.cache_size, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), " KB\n");
                
                strcpy(out + strlen(out), "physical id\t: 0\n");
                strcpy(out + strlen(out), "siblings\t: ");
                itoa(cpu_count, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "core id\t\t: ");
                itoa(i, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "cpu cores\t: ");
                itoa(cpu_count, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "apicid\t\t: ");
                itoa(i, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "initial apicid\t: ");
                itoa(i, buf);
                strcpy(out + strlen(out), buf);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "fpu\t\t: yes\n");
                strcpy(out + strlen(out), "fpu_exception\t: yes\n");
                
                strcpy(out + strlen(out), "cpuid level\t: 13\n");
                
                strcpy(out + strlen(out), "wp\t\t: yes\n");
                
                strcpy(out + strlen(out), "flags\t\t: ");
                strcpy(out + strlen(out), flags);
                strcpy(out + strlen(out), "\n");
                
                strcpy(out + strlen(out), "bugs\t\t: \n");
                strcpy(out + strlen(out), "bogomips\t: 4800.00\n");
                
                if (i < cpu_count - 1) {
                    strcpy(out + strlen(out), "\n");
                }
            }
        } else if (strcmp(h->type, "datetime") == 0) {
            extern void rtc_get_datetime(int *year, int *month, int *day, int *hour, int *minute, int *second);
            int y, m, d, h_val, min, s;
            rtc_get_datetime(&y, &m, &d, &h_val, &min, &s);
            
            char buf[16];
            itoa(y, buf);
            strcpy(out, buf);
            strcpy(out + strlen(out), "-");
            if (m < 10) strcpy(out + strlen(out), "0");
            itoa(m, buf);
            strcpy(out + strlen(out), buf);
            strcpy(out + strlen(out), "-");
            if (d < 10) strcpy(out + strlen(out), "0");
            itoa(d, buf);
            strcpy(out + strlen(out), buf);
            strcpy(out + strlen(out), " ");
            if (h_val < 10) strcpy(out + strlen(out), "0");
            itoa(h_val, buf);
            strcpy(out + strlen(out), buf);
            strcpy(out + strlen(out), ":");
            if (min < 10) strcpy(out + strlen(out), "0");
            itoa(min, buf);
            strcpy(out + strlen(out), buf);
            strcpy(out + strlen(out), ":");
            if (s < 10) strcpy(out + strlen(out), "0");
            itoa(s, buf);
            strcpy(out + strlen(out), buf);
            strcpy(out + strlen(out), "\n");
        } else if (strcmp(h->type, "meminfo") == 0) {
            extern MemStats memory_get_stats(void);
            MemStats stats = memory_get_stats();
            char m_s[32];
            
            strcpy(out, "MemTotal:\t");
            itoa(stats.total_memory / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "MemFree:\t");
            itoa(stats.available_memory / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "MemAvailable:\t");
            itoa(stats.available_memory / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "Buffers:\t0 kB\n");
            strcpy(out + strlen(out), "Cached:\t\t0 kB\n");
            
            strcpy(out + strlen(out), "MemUsed:\t");
            itoa(stats.used_memory / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "MemPeak:\t");
            itoa(stats.peak_memory_used / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "SwapTotal:\t0 kB\n");
            strcpy(out + strlen(out), "SwapFree:\t0 kB\n");
            
            strcpy(out + strlen(out), "Dirty:\t\t0 kB\n");
            strcpy(out + strlen(out), "Writeback:\t0 kB\n");
            strcpy(out + strlen(out), "AnonPages:\t");
            itoa(stats.used_memory / 1024, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), " kB\n");
            
            strcpy(out + strlen(out), "Mapped:\t\t0 kB\n");
            strcpy(out + strlen(out), "Shmem:\t\t0 kB\n");
            
            strcpy(out + strlen(out), "Blocks:\t\t");
            itoa(stats.allocated_blocks, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), "\n");
            
            strcpy(out + strlen(out), "FreeBlocks:\t");
            itoa(stats.free_blocks, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), "\n");
            
            strcpy(out + strlen(out), "Fragmentation:\t");
            itoa(stats.fragmentation_percent, m_s);
            strcpy(out + strlen(out), m_s);
            strcpy(out + strlen(out), "%\n");
        } else if (strcmp(h->type, "devices") == 0) {
            extern int disk_get_count(void);
            extern Disk* disk_get_by_index(int index);
            int dcount = disk_get_count();
            out[0] = '\0';
            
            strcpy(out, "Character devices:\n");
            strcpy(out + strlen(out), "  1 mem\n");
            strcpy(out + strlen(out), "  4 tty\n");
            strcpy(out + strlen(out), "  5 cua\n");
            strcpy(out + strlen(out), "  7 vcs\n");
            strcpy(out + strlen(out), "  8 stdin\n");
            strcpy(out + strlen(out), " 13 input\n");
            strcpy(out + strlen(out), " 14 sound\n");
            strcpy(out + strlen(out), " 29 fb\n");
            strcpy(out + strlen(out), "189 usb\n\n");
            
            strcpy(out + strlen(out), "Block devices:\n");
            for (int i = 0; i < dcount; i++) {
                Disk *d = disk_get_by_index(i);
                if (d && !d->is_partition) {
                    strcpy(out + strlen(out), "  8 ");
                    strcpy(out + strlen(out), d->devname);
                    strcpy(out + strlen(out), "\n");
                }
            }
            strcpy(out + strlen(out), " 11 sr\n");
            strcpy(out + strlen(out), "253 virtblk\n");
        }
    }
 else {
        process_t *proc = process_get_by_pid(h->pid);
        if (!proc) { kfree(out); return -1; }

        if (strcmp(h->type, "name") == 0 || strcmp(h->type, "cmdline") == 0) {
            strcpy(out, proc->name);
            strcpy(out + strlen(out), "\n");
        } else if (strcmp(h->type, "cwd") == 0) {
            strcpy(out, proc->cwd);
            strcpy(out + strlen(out), "\n");
        } else if (strcmp(h->type, "status") == 0) {
            strcpy(out, "Name: ");
            strcpy(out + strlen(out), proc->name);
            strcpy(out + strlen(out), "\nPID: ");
            char pid_s[16]; itoa(proc->pid, pid_s);
            strcpy(out + strlen(out), pid_s);
            strcpy(out + strlen(out), "\nState: RUNNING\nMemory: ");
            uint64_t mem_val = proc->used_memory;
            if (h->pid == 0) {
                extern MemStats memory_get_stats(void);
                mem_val = memory_get_stats().used_memory;
            }
            char mem_s[32]; itoa(mem_val / 1024, mem_s);
            strcpy(out + strlen(out), mem_s);
            strcpy(out + strlen(out), " KB\nTicks: ");
            char tick_s[32]; itoa(proc->ticks, tick_s);
            strcpy(out + strlen(out), tick_s);
            strcpy(out + strlen(out), "\nIdle: ");
            strcpy(out + strlen(out), proc->is_idle ? "1" : "0");
            strcpy(out + strlen(out), "\n");
        }
    }

    int len = strlen(out);
    if (h->offset >= len) { kfree(out); return 0; }

    int to_copy = len - h->offset;
    if (to_copy > size) to_copy = size;

    memcpy(buf, out + h->offset, to_copy);
    h->offset += to_copy;
    kfree(out);
    return to_copy;
}

int procfs_write(void *fs_private, void *handle, const void *buf, int size) {
    procfs_handle_t *h = (procfs_handle_t*)handle;
    if (!h || h->pid == 0xFFFFFFFF) return -1;

    if (strcmp(h->type, "signal") == 0) {
        char cmd[16];
        int to_copy = size < 15 ? size : 15;
        memcpy(cmd, buf, to_copy);
        cmd[to_copy] = 0;

        if (strcmp(cmd, "9") == 0 || strcmp(cmd, "kill") == 0) {
            process_t *proc = process_get_by_pid(h->pid);
            if (proc && proc->pid != 0) {
                process_terminate(proc);
                return size;
            }
        }
    }

    return -1;
}

int procfs_readdir(void *fs_private, const char *path, vfs_dirent_t *entries, int max, int offset) {
    if (path[0] == '/') path++;

    int out = 0;
    int found_so_far = 0;

    if (path[0] == '\0') {
        const char *static_files[] = {
            "version", "uptime", "cpuinfo", "meminfo", "datetime", "devices"
        };
        for (int i = 0; i < 6; i++) {
            if (found_so_far >= offset) {
                strcpy(entries[out].name, static_files[i]);
                entries[out].is_directory = 0;
                entries[out].size = 0;
                out++;
                if (out >= max) return out;
            }
            found_so_far++;
        }

        extern process_t processes[];
        for (int i = 0; i < 16; i++) {
            if (processes[i].pid != 0xFFFFFFFF) {
                if (found_so_far >= offset) {
                    itoa(processes[i].pid, entries[out].name);
                    entries[out].is_directory = 1;
                    entries[out].size = 0;
                    out++;
                    if (out >= max) return out;
                }
                found_so_far++;
            }
        }
        return out;
    }

    if (path[0] >= '0' && path[0] <= '9') {
        const char *pid_files[] = {
            "name", "status", "cmdline", "cwd", "signal"
        };
        for (int i = 0; i < 5; i++) {
            if (found_so_far >= offset) {
                strcpy(entries[out].name, pid_files[i]);
                entries[out].is_directory = 0;
                entries[out].size = 0;
                out++;
                if (out >= max) return out;
            }
            found_so_far++;
        }
        return out;
    }

    return 0;
}

bool procfs_exists(void *fs_private, const char *path) {
    if (path[0] == '/') path++;
    if (path[0] == '\0') return true;

    if (path[0] >= '0' && path[0] <= '9') {
        char pid_str[16];
        int i = 0;
        while (path[i] && path[i] != '/' && i < 15) {
            pid_str[i] = path[i];
            i++;
        }
        pid_str[i] = 0;
        uint32_t pid = atoi(pid_str);
        if (process_get_by_pid(pid)) return true;
    }

    if (strcmp(path, "version") == 0 || strcmp(path, "uptime") == 0) return true;
    if (strcmp(path, "cpuinfo") == 0 || strcmp(path, "meminfo") == 0) return true;
    if (strcmp(path, "datetime") == 0 || strcmp(path, "devices") == 0) return true;

    return false;
}

bool procfs_is_dir(void *fs_private, const char *path) {
    if (path[0] == '/') path++;
    if (path[0] == '\0') return true;

    if (path[0] >= '0' && path[0] <= '9') {
        int i = 0;
        while (path[i] && path[i] != '/') i++;
        if (path[i] == '\0') return true; 
        return false; 
    }

    return false; 
}

static int procfs_statfs(void *fs_private, vfs_statfs_t *stat) {
    (void)fs_private;
    stat->total_blocks = 0;
    stat->free_blocks = 0;
    stat->block_size = 512;
    return 0;
}

vfs_fs_ops_t procfs_ops = {
    .open = procfs_open,
    .close = procfs_close,
    .read = procfs_read,
    .write = procfs_write,
    .readdir = procfs_readdir,
    .exists = procfs_exists,
    .is_dir = procfs_is_dir,
    .statfs = procfs_statfs
};

vfs_fs_ops_t* procfs_get_ops(void) {
    return &procfs_ops;
}
