// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "process.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "io.h"
#include "platform.h"
#include "slab.h"
#include "elf.h"
#include "vfs.h"
#include "spinlock.h"
#include "smp.h"
#include "lapic.h"
#include "unix_socket.h"
#include "shm.h"
#include "kutils.h"
#include "fpu.h"
#include "vmm.h"

#define MSR_FS_BASE 0xC0000100


extern void cmd_write(const char *str);
extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);

#define MAX_CPUS_SCHED 32
#define PID_HASH_BUCKETS 128

static process_t *pid_hash_table[PID_HASH_BUCKETS] = {NULL};
static spinlock_t process_table_lock = SPINLOCK_INIT;
static spinlock_t runqueue_lock = SPINLOCK_INIT;

static volatile uint32_t next_pid = 0;

uint32_t reaper_pid = 0; 

static void process_cleanup_inner(process_t *proc);
static void process_init_signal_state(process_t *proc);
extern void poll_cleanup(process_t *proc);

static inline process_t *process_alloc_struct(void) {
    process_t *p = (process_t *)kmalloc(sizeof(process_t));
    if (p) memset(p, 0, sizeof(process_t));
    return p;
}

static inline void process_free_struct(process_t *p) {
    if (p) kfree(p);
}

static __attribute__((aligned(16))) uint8_t clean_fxsave_template[512];

static uint32_t pid_hash(uint32_t pid) {
    return pid % PID_HASH_BUCKETS;
}

static void pid_table_insert(process_t *proc) {
    if (!proc) return;
    uint64_t rflags = spinlock_acquire_irqsave(&process_table_lock);
    uint32_t idx = pid_hash(proc->pid);
    proc->pid_hash_next = pid_hash_table[idx];
    pid_hash_table[idx] = proc;
    spinlock_release_irqrestore(&process_table_lock, rflags);
}

static process_t *pid_table_remove_unlocked(uint32_t pid) {
    uint32_t idx = pid_hash(pid);
    process_t *curr = pid_hash_table[idx];
    process_t *prev = NULL;
    process_t *found = NULL;
    while (curr) {
        if (curr->pid == pid) {
            found = curr;
            if (prev) prev->pid_hash_next = curr->pid_hash_next;
            else pid_hash_table[idx] = curr->pid_hash_next;
            curr->pid_hash_next = NULL;
            break;
        }
        prev = curr;
        curr = curr->pid_hash_next;
    }
    return found;
}

static process_t *pid_table_remove(uint32_t pid) {
    uint64_t rflags = spinlock_acquire_irqsave(&process_table_lock);
    process_t *found = pid_table_remove_unlocked(pid);
    spinlock_release_irqrestore(&process_table_lock, rflags);
    return found;
}

void process_hold(process_t *proc) {
    if (!proc) return;
    __atomic_fetch_add(&proc->refcount, 1, __ATOMIC_RELAXED);
}

void process_put(process_t *proc) {
    if (!proc) return;
    if (__atomic_fetch_sub(&proc->refcount, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (proc->kernel_stack_alloc) {
            kfree_null(proc->kernel_stack_alloc);
            proc->kernel_stack_alloc = NULL;
        }
        bool should_destroy_pml4 = true;
        if (proc->pml4_refcount) {
            int refs = __atomic_fetch_sub(proc->pml4_refcount, 1, __ATOMIC_RELEASE);
            if (refs > 1) {
                should_destroy_pml4 = false;
            } else {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                kfree_null(proc->pml4_refcount);
                proc->pml4_refcount = NULL;
            }
        }
        if (proc->user_stack_alloc) {
            if (proc->pml4_phys) {
                extern void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);
                uint64_t stack_top = 0x800000;
                uint64_t stack_size = 262144;
                for (uint64_t off = 0; off < stack_size; off += 4096) {
                    paging_unmap_page(proc->pml4_phys, stack_top - stack_size + off);
                }
            }
            kfree_null(proc->user_stack_alloc);
            proc->user_stack_alloc = NULL;
        }
        if (proc->vmm_space && should_destroy_pml4) {
            vmm_destroy_space(proc->vmm_space);
            proc->vmm_space = NULL;
            proc->pml4_phys = 0;
        } else if (proc->pml4_phys && should_destroy_pml4) {
            extern void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped);
            paging_destroy_user_pml4_phys(proc->pml4_phys, true);
            proc->pml4_phys = 0;
        }
        process_free_struct(proc);
        proc = NULL;
    }
}


process_t* process_get_by_pid(uint32_t pid) {
    uint64_t rflags = spinlock_acquire_irqsave(&process_table_lock);
    uint32_t idx = pid_hash(pid);
    process_t *curr = pid_hash_table[idx];
    process_t *found = NULL;
    while (curr) {
        if (curr->pid == pid) {
            found = curr;
            process_hold(found);
            break;
        }
        curr = curr->pid_hash_next;
    }
    spinlock_release_irqrestore(&process_table_lock, rflags);
    return found;
}

void process_table_for_each(void (*cb)(process_t *proc, void *arg), void *arg) {
    if (!cb) return;
    uint64_t rflags = spinlock_acquire_irqsave(&process_table_lock);
    for (int i = 0; i < PID_HASH_BUCKETS; i++) {
        process_t *curr = pid_hash_table[i];
        while (curr) {
            cb(curr, arg);
            curr = curr->pid_hash_next;
        }
    }
    spinlock_release_irqrestore(&process_table_lock, rflags);
}

typedef struct {
    uint32_t *pids;
    int count;
    int max;
} get_pids_arg_t;

static void collect_pids_cb(process_t *proc, void *arg) {
    get_pids_arg_t *a = (get_pids_arg_t *)arg;
    if (a->count < a->max) {
        a->pids[a->count++] = proc->pid;
    }
}

int process_get_all_pids(uint32_t *pids_out, int max_pids) {
    if (!pids_out || max_pids <= 0) return 0;
    get_pids_arg_t a = { .pids = pids_out, .count = 0, .max = max_pids };
    process_table_for_each(collect_pids_cb, &a);
    return a.count;
}



void process_close_fd_inner(process_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd]) {
        return;
    }

    if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
        process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
        if (ref) {
            if (__atomic_fetch_sub(&ref->refs, 1, __ATOMIC_RELEASE) == 1) {
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (ref->file) vfs_close((vfs_file_t *)ref->file);
                kfree_null(ref);
            }
        }
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ || proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
        process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
        if (pipe) {
            if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ)
                __atomic_fetch_sub(&pipe->readers, 1, __ATOMIC_RELEASE);
            else
                __atomic_fetch_sub(&pipe->writers, 1, __ATOMIC_RELEASE);
            if (__atomic_load_n(&pipe->readers, __ATOMIC_ACQUIRE) <= 0 &&
                __atomic_load_n(&pipe->writers, __ATOMIC_ACQUIRE) <= 0) {
                kfree_null(pipe);
            }
        }
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
        process_socket_release((process_fd_socket_t *)proc->fds[fd]);
    }

    proc->fds[fd] = NULL;
    proc->fd_kind[fd] = PROC_FD_KIND_NONE;
    proc->fd_flags[fd] = 0;
}



process_fd_socket_t *process_socket_create(void) {
    process_fd_socket_t *sock = (process_fd_socket_t *)kmalloc(sizeof(*sock));
    if (!sock) return NULL;
    memset(sock, 0, sizeof(*sock));
    sock->refs = 1;
    sock->lock = SPINLOCK_INIT;
    sock->backlog_max = 128;
    sockbuf_init(&sock->rx_sb, 64 * 1024);
    sockbuf_init(&sock->tx_sb, 64 * 1024);
    wait_queue_init(&sock->accept_waitq);
    wait_queue_init(&sock->rx_waitq);
    return sock;
}

void process_socket_addref(process_fd_socket_t *sock) {
    if (!sock) return;
    __atomic_fetch_add(&sock->refs, 1, __ATOMIC_RELAXED);
}

void process_socket_release(process_fd_socket_t *sock) {
    if (!sock) return;
    if (__atomic_fetch_sub(&sock->refs, 1, __ATOMIC_RELEASE) > 1) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (sock->domain == 1 || sock->unpcb != NULL) {
        extern void unix_socket_close(void *sock);
        unix_socket_close(sock);
    } else {
        extern void network_socket_close(process_fd_socket_t *sock);
        network_socket_close(sock);
    }

    accept_queue_entry_t *curr = sock->accept_head;
    while (curr) {
        accept_queue_entry_t *next = curr->next;
        if (curr->client_sock) {
            process_socket_release((process_fd_socket_t *)curr->client_sock);
        }
        kfree_null(curr);
        curr = next;
    }
    sock->accept_head = NULL;
    sock->accept_tail = NULL;
    sock->accept_queue_count = 0;

    sockbuf_destroy(&sock->rx_sb);
    sockbuf_destroy(&sock->tx_sb);
    kfree_null(sock);
}

static void process_init_signal_state(process_t *proc) {
    if (!proc) return;
    proc->signal_mask = 0;
    proc->signal_pending = 0;
    for (int i = 0; i < MAX_SIGNALS; i++) {
        proc->signal_handlers[i] = 0;
        proc->signal_action_mask[i] = 0;
        proc->signal_action_flags[i] = 0;
    }
}

void process_init(void) {
    asm volatile("fninit");
    asm volatile("fxsave %0" : "=m"(clean_fxsave_template));

    process_t *kernel_proc = process_alloc_struct();
    if (!kernel_proc) return;
    kernel_proc->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    kernel_proc->refcount = 10000;
    kernel_proc->running_cpu = 0;
    kernel_proc->is_user = false;
    kernel_proc->is_idle = true;
    kernel_proc->state = PROC_STATE_RUNNING;
    kernel_proc->tty_id = -1;
    kernel_proc->kill_pending = false;

    kernel_proc->pml4_phys = paging_get_kernel_pml4_phys();
    void *kstack = kmalloc_aligned(KERNEL_STACK_SIZE, KERNEL_STACK_ALIGNMENT);
    if (kstack) {
        memset(kstack, 0, KERNEL_STACK_SIZE);
        kernel_proc->kernel_stack_alloc = kstack;
        kernel_proc->kernel_stack = (uint64_t)kstack + KERNEL_STACK_SIZE;

        extern void work_queue_drain_loop(void);
        uint64_t *stack_ptr = (uint64_t *)kernel_proc->kernel_stack;
        *(--stack_ptr) = 0x10;                                // SS (Kernel Data)
        *(--stack_ptr) = kernel_proc->kernel_stack;           // RSP
        *(--stack_ptr) = 0x202;                               // RFLAGS
        *(--stack_ptr) = 0x08;                                // CS (Kernel Code)
        *(--stack_ptr) = (uint64_t)work_queue_drain_loop;     // RIP
        *(--stack_ptr) = 0;                                   // err_code
        *(--stack_ptr) = 0;                                   // int_no
        for (int i = 0; i < 15; i++) *(--stack_ptr) = 0;      // 15 GPRs
        stack_ptr = (uint64_t *)((uint64_t)stack_ptr - 512);  // fxsave_region
        memcpy((void *)stack_ptr, clean_fxsave_template, 512);
        kernel_proc->rsp = (uint64_t)stack_ptr;
    } else {
        kernel_proc->kernel_stack = 0;
        kernel_proc->rsp = 0;
    }
    kernel_proc->fpu_initialized = true;

    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        kernel_proc->fds[i] = NULL;
        kernel_proc->fd_kind[i] = 0;
        kernel_proc->fd_flags[i] = 0;
    }
    kernel_proc->parent_pid = 0;
    kernel_proc->pgid = 0;
    kernel_proc->exited = false;
    kernel_proc->exit_status = 0;
    process_init_signal_state(kernel_proc);

    memcpy(kernel_proc->name, "kernel", 7);
    kernel_proc->ticks = 0;
    kernel_proc->used_memory = 32768;
    kernel_proc->fs_base = 0;

    kernel_proc->next = kernel_proc; // Circular linked list
    kernel_proc->cpu_affinity = 0;   // Kernel always on BSP
    memset(kernel_proc->cwd, 0, 1024);
    kernel_proc->cwd[0] = '/';
    memset(&kernel_proc->poll_table, 0, sizeof(kernel_proc->poll_table));
    
    pid_table_insert(kernel_proc);
    process_set_current_for_cpu(0, kernel_proc);
    process_set_idle_for_cpu(0, kernel_proc);

    /* job_applications zombie reaper is now a userspace daemon */
}

process_t* process_create(void (*entry_point)(void), bool is_user) {
    if (!entry_point && !is_user) {
        extern void work_queue_drain_loop(void);
        entry_point = work_queue_drain_loop;
    }

    process_t *new_proc = process_alloc_struct();
    if (!new_proc) {
        return NULL;
    }

    process_t *parent = process_get_current();

    new_proc->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    new_proc->refcount = 1;
    new_proc->running_cpu = -1;
    new_proc->is_user = is_user;
    new_proc->state = PROC_STATE_RUNNING;
    new_proc->tty_id = -1;
    new_proc->kill_pending = false;
    new_proc->parent_pid = parent ? parent->pid : 0;
    new_proc->pgid = parent ? parent->pgid : new_proc->pid;
    new_proc->exited = false;
    new_proc->exit_status = 0;
    new_proc->fs_base = 0;
    process_init_signal_state(new_proc);
    
    if (parent) {
        memcpy(new_proc->cwd, parent->cwd, 1024);
        new_proc->tty_id = parent->tty_id;
    } else {
        memset(new_proc->cwd, 0, 1024);
        new_proc->cwd[0] = '/';
    }
    
    // 1. Setup Page Table
    if (is_user) {
        new_proc->pml4_phys = paging_create_user_pml4_phys();
        new_proc->pml4_refcount = (int *)kmalloc(sizeof(int));
        if (new_proc->pml4_refcount) *new_proc->pml4_refcount = 1;
    } else {
        new_proc->pml4_phys = paging_get_kernel_pml4_phys();
        new_proc->pml4_refcount = NULL;
    }
    
    if (!new_proc->pml4_phys) {
        process_put(new_proc);
        return NULL;
    }
    
    void* user_stack = kmalloc_aligned(131072, 4096);
    void* kernel_stack = kmalloc_aligned(32768, 32768); // Needed for when user interrupts to Ring 0
    if (!user_stack || !kernel_stack) {
        kfree_null(user_stack);
        kfree_null(kernel_stack);
        process_put(new_proc);
        return NULL;
    }
    memset(user_stack, 0, 131072);
    memset(kernel_stack, 0, 32768);
    
    if (is_user) {
        for (int i = 0; i < 32; i++) {
            if (!paging_map_page(new_proc->pml4_phys, 0x800000 + i*4096, v2p((uint64_t)user_stack + i*4096), PT_PRESENT | PT_RW | PT_USER)) {
                kfree_null(user_stack);
                kfree_null(kernel_stack);
                process_put(new_proc);
                return NULL;
            }
        }

        // Allocate code page aligned and copy code
        void* code = kmalloc_aligned(4096, 4096);
        if (!code) {
            kfree_null(user_stack);
            kfree_null(kernel_stack);
            process_put(new_proc);
            return NULL;
        }
        for(int i=0; i<128; i++) ((uint8_t*)code)[i] = ((uint8_t*)entry_point)[i];

        if (!paging_map_page(new_proc->pml4_phys, 0x400000, v2p((uint64_t)code), PT_PRESENT | PT_RW | PT_USER)) {
            kfree_null(code);
            kfree_null(user_stack);
            kfree_null(kernel_stack);
            process_put(new_proc);
            return NULL;
        }
        
        // Build initial stack frame for iretq
        // Stack grows down, start at top
        uint64_t* stack_ptr = (uint64_t*)((uint64_t)kernel_stack + 32768);
        
        *(--stack_ptr) = 0x1B;          // SS (User Data)
        *(--stack_ptr) = 0x800000 + 131072; // RSP
        *(--stack_ptr) = 0x202;         // RFLAGS (IF=1)
        *(--stack_ptr) = 0x23;          // CS (User Code)
        *(--stack_ptr) = 0x400000;      // RIP
        *(--stack_ptr) = 0;             // err_code
        *(--stack_ptr) = 0;             // int_no
        
        // Push 15 zeros for general purpose registers (r15 -> rax)
        for (int i = 0; i < 15; i++) *(--stack_ptr) = 0;
        
        // Push 512 bytes for SSE/FPU state (fxsave_region)
        stack_ptr = (uint64_t*)((uint64_t)stack_ptr - 512);
        memcpy((void *)stack_ptr, clean_fxsave_template, 512);

        new_proc->kernel_stack = (uint64_t)kernel_stack + 32768;
        new_proc->kernel_stack_alloc = kernel_stack;
        new_proc->user_stack_alloc = user_stack;
        new_proc->rsp = (uint64_t)stack_ptr;
    } else {
        // Kernel thread
        uint64_t* stack_ptr = (uint64_t*)((uint64_t)kernel_stack + 32768);
        *(--stack_ptr) = 0x10;          // SS (Kernel Data)
        *(--stack_ptr) = (uint64_t)kernel_stack + 32768;
        *(--stack_ptr) = 0x202;         // RFLAGS
        *(--stack_ptr) = 0x08;          // CS (Kernel Code)
        *(--stack_ptr) = (uint64_t)entry_point; // RIP
        *(--stack_ptr) = 0;             // err_code
        *(--stack_ptr) = 0;             // int_no
        
        // Push 15 zeros for general purpose registers (r15 -> rax)
        for (int i = 0; i < 15; i++) *(--stack_ptr) = 0;
        
        // Push 512 bytes for SSE/FPU state (fxsave_region)
        stack_ptr = (uint64_t*)((uint64_t)stack_ptr - 512);
        memcpy((void *)stack_ptr, clean_fxsave_template, 512);

        new_proc->kernel_stack = (uint64_t)kernel_stack + 32768;
        new_proc->kernel_stack_alloc = kernel_stack;
        new_proc->rsp = (uint64_t)stack_ptr;
        kfree_null(user_stack); // Unused for kernel threads
    }

    new_proc->fpu_initialized = true;
    new_proc->cpu_affinity = 0; // Kernel processes stay on BSP
    if (!is_user && new_proc->name[0] == '\0') {
        memcpy(new_proc->name, "kworker", 8);
    }
    
    // Add to linked list
    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);
    process_t *head = pid_hash_table[0] ? pid_hash_table[0] : process_get_current_for_cpu(0);
    if (head) {
        new_proc->next = head->next;
        head->next = new_proc;
    } else {
        new_proc->next = new_proc;
    }
    spinlock_release_irqrestore(&runqueue_lock, rflags);

    pid_table_insert(new_proc);
    return new_proc;
}

void process_add_elf_segment(process_t *proc, void *ptr, uint64_t vaddr, size_t size) {
    if (!proc || !ptr) return;
    if (proc->elf_segment_count < 4) {
        proc->elf_segments[proc->elf_segment_count].ptr = ptr;
        proc->elf_segments[proc->elf_segment_count].vaddr = vaddr;
        proc->elf_segments[proc->elf_segment_count].size = size;
        proc->elf_segment_count++;
    }
    if (proc->vmm_space) {
        vm_area_t *vma = vma_create(vaddr, vaddr + size, VMA_TYPE_ANON,
                                    VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_EXEC);
        if (vma) {
            vma_insert(&proc->vmm_space->vma_head, &proc->vmm_space->vma_tree, vma);
        }
    }
}

process_t* process_create_elf(const char* filepath, const char* args_str, bool terminal_proc, int tty_id) {
    process_t *new_proc = process_alloc_struct();
    if (!new_proc) return NULL;

    new_proc->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    new_proc->refcount = 1;
    new_proc->running_cpu = -1;
    new_proc->is_user = true;
    new_proc->state = PROC_STATE_RUNNING;
    new_proc->elf_segment_count = 0;
    new_proc->fs_base = 0;
    new_proc->cpu_affinity = CPU_AFFINITY_ANY;
    
    // 1. Setup Page Table
    new_proc->vmm_space = vmm_create_space();
    if (new_proc->vmm_space) {
        new_proc->pml4_phys = new_proc->vmm_space->mmu_ctx->pml4_phys;
    } else {
        new_proc->pml4_phys = paging_create_user_pml4_phys();
    }
    if (!new_proc->pml4_phys) {
        process_put(new_proc);
        return NULL;
    }
    new_proc->pml4_refcount = (int *)kmalloc(sizeof(int));
    if (new_proc->pml4_refcount) *new_proc->pml4_refcount = 1;

    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        new_proc->fds[i] = NULL;
        new_proc->fd_kind[i] = 0;
        new_proc->fd_flags[i] = 0;
    }

    process_t *parent = process_get_current();
    if (parent) {
        for (int i = 0; i < 3; i++) {
            if (parent->fds[i]) {
                if (parent->fd_kind[i] == PROC_FD_KIND_FILE) {
                    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)parent->fds[i];
                    if (ref) {
                        vfs_file_t *vf = (vfs_file_t *)ref->file;
                        if (tty_id < 0 && vf && vf->is_device && vf->device_type == DEVICE_TYPE_TTY) {
                            continue;
                        }
                    }
                }
                new_proc->fds[i] = parent->fds[i];
                new_proc->fd_kind[i] = parent->fd_kind[i];
                new_proc->fd_flags[i] = parent->fd_flags[i];
                
                if (new_proc->fd_kind[i] == PROC_FD_KIND_FILE) {
                    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)new_proc->fds[i];
                    if (ref) __atomic_fetch_add(&ref->refs, 1, __ATOMIC_RELAXED);
                } else if (new_proc->fd_kind[i] == PROC_FD_KIND_PIPE_READ) {
                    process_fd_pipe_t *pipe = (process_fd_pipe_t *)new_proc->fds[i];
                    if (pipe) __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELAXED);
                } else if (new_proc->fd_kind[i] == PROC_FD_KIND_PIPE_WRITE) {
                    process_fd_pipe_t *pipe = (process_fd_pipe_t *)new_proc->fds[i];
                    if (pipe) __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELAXED);
                } else if (new_proc->fd_kind[i] == PROC_FD_KIND_SOCKET) {
                    process_socket_addref((process_fd_socket_t *)new_proc->fds[i]);
                }
            }
        }
    }

    // Always set up TTY FDs if a TTY is provided
    if (tty_id >= 0) {
        char tty_path[24];
        extern void strcpy(char *dest, const char *src);
        extern void itoa(int n, char *buf);
        if (tty_id >= 1024) {
            strcpy(tty_path, "/dev/pts/");
            itoa(tty_id - 1024, tty_path + 9);
        } else {
            strcpy(tty_path, "/dev/tty");
            itoa(tty_id + 1, tty_path + 8);
        }
        
        vfs_file_t *f = vfs_open(tty_path, "rw");
        if (f) {
            process_fd_file_ref_t *ref = kmalloc(sizeof(process_fd_file_ref_t));
            if (ref) {
                ref->file = f;
                ref->refs = 0;
                for (int i = 0; i < 3; i++) {
                    if (new_proc->fds[i]) {
                        // Close inherited FD if any
                        process_close_fd_inner(new_proc, i);
                    }
                    new_proc->fds[i] = ref;
                    new_proc->fd_kind[i] = PROC_FD_KIND_FILE;
                    new_proc->fd_flags[i] = (i == 0) ? 0 : 1;
                    ref->refs++;
                }
            }
        }
    }


    new_proc->heap_start = 0x20000000; // 512MB mark
    new_proc->heap_end = 0x20000000;
    new_proc->mmap_current = 0x50000000;
    new_proc->mmap_allocation_count = 0;
    for (int i = 0; i < 16; i++) new_proc->mmap_allocations[i] = NULL;
    new_proc->shm_mapping_count = 0;
    for (int i = 0; i < 32; i++) {
        new_proc->shm_mappings[i].addr = 0;
        new_proc->shm_mappings[i].length = 0;
        new_proc->shm_mappings[i].seg = NULL;
    }
    new_proc->is_terminal_proc = terminal_proc;
    new_proc->tty_id = tty_id;
    new_proc->kill_pending = false;
    new_proc->exited = false;
    new_proc->exit_status = 0;
    new_proc->is_cloned_child = false;
    process_init_signal_state(new_proc);

    if (parent) {
        memcpy(new_proc->cwd, parent->cwd, 1024);
        new_proc->parent_pid = parent->pid;
        new_proc->pgid = parent->pgid;
    } else {
        memset(new_proc->cwd, 0, 1024);
        new_proc->cwd[0] = '/';
        new_proc->parent_pid = 0;
        new_proc->pgid = new_proc->pid;
    }

    // 2. Load ELF executable
    size_t elf_load_size = 0;
    uint64_t phdr_vaddr = 0, phdr_num = 0;
    uint64_t entry_point = elf_load(filepath, new_proc->pml4_phys, &elf_load_size, new_proc, &phdr_vaddr, &phdr_num);
    if (entry_point == 0) {
        serial_write("[PROC] Failed to load ELF: ");
        serial_write(filepath);
        serial_write("\n");
        return NULL;
    }

    // Set process name from filepath
    int last_slash = -1;
    for (int i = 0; filepath[i]; i++) if (filepath[i] == '/') last_slash = i;
    const char *filename = (last_slash == -1) ? filepath : (filepath + last_slash + 1);
    int ni = 0;
    while (filename[ni] && ni < 63) {
        new_proc->name[ni] = filename[ni];
        ni++;
    }
    new_proc->name[ni] = 0;
    new_proc->ticks = 0;

    // 3. Allocate generic User stack and Kernel stack for interrupts
    // Increase to 256KB to prevent stack smashing on heavy networking
    const size_t user_stack_size = 262144;
    void* stack = kmalloc_aligned(user_stack_size, 4096);
    void* kernel_stack = kmalloc_aligned(KERNEL_STACK_SIZE, KERNEL_STACK_ALIGNMENT); 
    if (!stack || !kernel_stack) {
        kfree_null(stack);
        kfree_null(kernel_stack);
        return NULL;
    }
    memset(stack, 0, user_stack_size);
    memset(kernel_stack, 0, KERNEL_STACK_SIZE); 
    
    // Map User stack to 0x800000
    for (uint64_t i = 0; i < (user_stack_size / 4096); i++) {
        if (!paging_map_page(new_proc->pml4_phys, 0x800000 - user_stack_size + (i * 4096), v2p((uint64_t)stack + (i * 4096)), PT_PRESENT | PT_RW | PT_USER)) {
            kfree_null(stack);
            kfree_null(kernel_stack);
            return NULL;
        }
    }

    if (new_proc->vmm_space) {
        vm_area_t *stack_vma = vma_create(0x800000 - user_stack_size, 0x800000, VMA_TYPE_ANON,
                                          VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_STACK);
        if (stack_vma) {
            vma_insert(&new_proc->vmm_space->vma_head, &new_proc->vmm_space->vma_tree, stack_vma);
        }

        new_proc->vmm_space->brk_start = new_proc->heap_start;
        new_proc->vmm_space->brk_current = new_proc->heap_end;
        vm_area_t *heap_vma = vma_create(new_proc->heap_start, new_proc->heap_end, VMA_TYPE_ANON,
                                         VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_HEAP);
        if (heap_vma) {
            vma_insert(&new_proc->vmm_space->vma_head, &new_proc->vmm_space->vma_tree, heap_vma);
        }
    }

    int argc = 1;
    char *args_buf = (char *)stack + user_stack_size;
    uint64_t user_args_buf = 0x800000;

    // Copy filepath as argv[0]
    int path_len = 0;
    while (filepath[path_len]) path_len++;
    args_buf -= (path_len + 1);
    user_args_buf -= (path_len + 1);
    for (int i = 0; i <= path_len; i++) args_buf[i] = filepath[i];
    
    uint64_t argv_ptrs[32];
    argv_ptrs[0] = user_args_buf;

    if (args_str) {
        int i = 0;
        while (args_str[i] && argc < 31) {
            // Skip spaces
            while (args_str[i] == ' ') i++;
            if (!args_str[i]) break;

            int arg_start = i;
            bool in_quotes = false;
            
            if (args_str[i] == '"') {
                in_quotes = true;
                i++;
                arg_start = i;
                while (args_str[i] && args_str[i] != '"') i++;
            } else {
                while (args_str[i] && args_str[i] != ' ') i++;
            }
            
            int arg_len = i - arg_start;

            args_buf -= (arg_len + 1);
            user_args_buf -= (arg_len + 1);
            
            for (int k = 0; k < arg_len; k++) {
                args_buf[k] = args_str[arg_start + k];
            }
            args_buf[arg_len] = '\0';
            
            argv_ptrs[argc++] = user_args_buf;
            
            if (in_quotes && args_str[i] == '"') i++; // Skip closing quote
        }
    }
    argv_ptrs[argc] = 0; // Null terminator for argv

    // Push System V ABI Stack Frame:
    // rsp -> [argc]
    //        [argv[0] ... argv[argc-1]]
    //        [NULL]
    //        [envp[0] = NULL]
    //        [auxv[0] key = AT_NULL]
    //        [auxv[0] val = 0]
    
    int total_elements = 1 + (argc + 1) + 1 + 14; 
    int total_size = total_elements * (int)sizeof(uint64_t);

    uint64_t target_sp = (user_args_buf - total_size) & ~15ULL;
    uint64_t current_user_sp = target_sp + total_size;
    
    args_buf = (char *)((uint64_t)stack + (current_user_sp - (0x800000 - user_stack_size)));

    // 1. Push AUXV (mlibc standard entries + AT_NULL terminator)
    args_buf -= 14 * sizeof(uint64_t);
    current_user_sp -= 14 * sizeof(uint64_t);
    uint64_t *user_auxv = (uint64_t *)args_buf;
    user_auxv[0] = 9;   user_auxv[1] = entry_point;    // AT_ENTRY
    user_auxv[2] = 6;   user_auxv[3] = 4096;           // AT_PAGESZ
    user_auxv[4] = 3;   user_auxv[5] = phdr_vaddr;     // AT_PHDR
    user_auxv[6] = 4;   user_auxv[7] = 56;             // AT_PHENT (sizeof(Elf64_Phdr))
    user_auxv[8] = 5;   user_auxv[9] = phdr_num;       // AT_PHNUM
    user_auxv[10] = 25; user_auxv[11] = user_args_buf; // AT_RANDOM
    user_auxv[12] = 0;  user_auxv[13] = 0;             // AT_NULL

    // 2. Push ENVP (empty)
    args_buf -= 1 * sizeof(uint64_t);
    current_user_sp -= 1 * sizeof(uint64_t);
    uint64_t *user_envp = (uint64_t *)args_buf;
    user_envp[0] = 0;

    // 3. Push ARGV
    int argv_size = (argc + 1) * sizeof(uint64_t);
    args_buf -= argv_size;
    current_user_sp -= argv_size;
    uint64_t *user_argv_array = (uint64_t *)args_buf;
    for (int i = 0; i <= argc; i++) {
        user_argv_array[i] = argv_ptrs[i];
    }
    uint64_t actual_argv_ptr = current_user_sp;

    // 4. Push ARGC
    args_buf -= 1 * sizeof(uint64_t);
    current_user_sp -= 1 * sizeof(uint64_t);
    uint64_t *user_argc = (uint64_t *)args_buf;
    user_argc[0] = (uint64_t)argc;

    // 4. Build Stack Frame for context switch via IRETQ
    uint64_t* stack_ptr = (uint64_t*)((uint64_t)kernel_stack + KERNEL_STACK_SIZE);
    *(--stack_ptr) = 0x1B;            // SS (User Mode Data)
    *(--stack_ptr) = current_user_sp; // RSP (Updated user stack pointer)
    *(--stack_ptr) = 0x202;           // RFLAGS (Interrupts Enabled)
    *(--stack_ptr) = 0x23;            // CS (User Mode Code)
    *(--stack_ptr) = entry_point;     // RIP
    *(--stack_ptr) = 0;               // err_code
    *(--stack_ptr) = 0;               // int_no
    // 15 General purpose registers
    *(--stack_ptr) = 0;                // RAX
    *(--stack_ptr) = 0;                // RBX
    *(--stack_ptr) = 0;                // RCX
    *(--stack_ptr) = 0;                // RDX
    *(--stack_ptr) = actual_argv_ptr;  // RSI = actual argv array
    *(--stack_ptr) = argc;             // RDI = argc
    *(--stack_ptr) = 0;                // RBP
    *(--stack_ptr) = 0;                // R8
    *(--stack_ptr) = 0;                // R9
    *(--stack_ptr) = 0;                // R10
    *(--stack_ptr) = 0;                // R11
    *(--stack_ptr) = 0;                // R12
    *(--stack_ptr) = 0;                // R13
    *(--stack_ptr) = 0;                // R14
    *(--stack_ptr) = 0;                // R15
    
    // Space for 512-byte fxsave_region
    stack_ptr = (uint64_t*)((uint64_t)stack_ptr - 512);
    // Initialize with a clean FPU state from template
    memcpy((void *)stack_ptr, clean_fxsave_template, 512);

    new_proc->kernel_stack = (uint64_t)kernel_stack + KERNEL_STACK_SIZE;
    new_proc->kernel_stack_alloc = kernel_stack;
    new_proc->user_stack_alloc = stack;
    new_proc->rsp = (uint64_t)stack_ptr;
    new_proc->used_memory = elf_load_size + user_stack_size + KERNEL_STACK_SIZE;

    new_proc->fpu_initialized = true;

    new_proc->cpu_affinity = CPU_AFFINITY_ANY;

    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);
    process_t *head = pid_hash_table[0] ? pid_hash_table[0] : process_get_current_for_cpu(0);
    if (head) {
        new_proc->next = head->next;
        head->next = new_proc;
    } else {
        new_proc->next = new_proc;
    }
    spinlock_release_irqrestore(&runqueue_lock, rflags);

    pid_table_insert(new_proc);

    serial_write("[PROC] Exec: ");
    serial_write(filepath);
    serial_write("\n");

    return new_proc;
}

process_t* process_get_current_for_cpu(uint32_t cpu_id) {
    cpu_state_t *cpu_state = smp_get_cpu(cpu_id);
    return cpu_state ? (process_t *)cpu_state->current_process : NULL;
}

void process_set_current_for_cpu(uint32_t cpu_id, process_t* p) {
    cpu_state_t *cpu_state = smp_get_cpu(cpu_id);
    if (cpu_state) {
        cpu_state->current_process = (struct process *)p;
    }
}

void process_set_idle_for_cpu(uint32_t cpu_id, process_t* p) {
    cpu_state_t *cpu_state = smp_get_cpu(cpu_id);
    if (cpu_state) {
        cpu_state->idle_process = (struct process *)p;
    }
}

process_t* process_get_idle_for_cpu(uint32_t cpu_id) {
    cpu_state_t *cpu_state = smp_get_cpu(cpu_id);
    return cpu_state ? (process_t *)cpu_state->idle_process : NULL;
}

process_t* process_get_current(void) {
    return (process_t *)current_cpu->current_process;
}

uint32_t process_get_current_pid(void) {
    process_t *p = process_get_current();
    return p ? p->pid : 0;
}

uint64_t process_schedule(uint64_t current_rsp) {
    uint32_t my_cpu = current_cpu->cpu_id;

    if (current_cpu->process_free_later) {
        process_put((process_t *)current_cpu->process_free_later);
        current_cpu->process_free_later = NULL;
        // are you happy now sassy dallas
    }

    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);

    process_t *cur = (process_t *)current_cpu->current_process;
    if (!cur) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return current_rsp;
    }

    if (cur->kill_pending && cur->pid != 0) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return process_terminate_current_with_status(cur->exit_status ? cur->exit_status : 1, current_rsp);
    }

    cur->rsp = current_rsp;
    cur->fs_base = rdmsr(MSR_FS_BASE);

    extern uint32_t get_ticks(void);
    uint32_t now = get_ticks();

    process_t *start = cur;
    process_t *next_proc = cur->next;
    process_t *chosen = NULL;

    if (next_proc) {
        do {
            bool matches_cpu = (next_proc->cpu_affinity == my_cpu || next_proc->cpu_affinity == CPU_AFFINITY_ANY);
            bool not_running_elsewhere = (next_proc->running_cpu == -1 || next_proc->running_cpu == (int)my_cpu);
            bool is_runnable = (!next_proc->kill_pending && !next_proc->exited && next_proc->state != PROC_STATE_ZOMBIE);

            if (matches_cpu && not_running_elsewhere && is_runnable) {
                if (next_proc->pid == 0 ||
                    (next_proc->state == PROC_STATE_RUNNING && (next_proc->sleep_until == 0 || next_proc->sleep_until <= now)) ||
                    (next_proc->state == PROC_STATE_BLOCKED && next_proc->sleep_until > 0 && next_proc->sleep_until <= now)) {
                    
                    if (next_proc->state == PROC_STATE_BLOCKED) {
                        next_proc->state = PROC_STATE_RUNNING;
                        next_proc->sleep_until = 0;
                    }
                    chosen = next_proc;
                    break;
                }
            }
            next_proc = next_proc->next;
        } while (next_proc != start && next_proc != NULL);
    }

    if (!chosen) {
        chosen = cur;
        if (chosen->state == PROC_STATE_ZOMBIE || chosen->state == PROC_STATE_BLOCKED || chosen->kill_pending) {
            process_t *kp = (process_t *)current_cpu->idle_process;
            if (kp) {
                chosen = kp;
            }
        }
    }

    if (cur != chosen) {
        cur->running_cpu = -1;
        chosen->running_cpu = (int)my_cpu;
        current_cpu->current_process = (struct process *)chosen;

        if (chosen->kernel_stack) {
            tss_set_stack_cpu(my_cpu, chosen->kernel_stack);
            current_cpu->kernel_syscall_stack = chosen->kernel_stack;
        }

        if (chosen->vmm_space) {
            __atomic_fetch_or(&chosen->vmm_space->active_cpus, (1ULL << my_cpu), __ATOMIC_SEQ_CST);
        }
        asm volatile("mfence" ::: "memory");
        paging_switch_directory(chosen->pml4_phys);
        asm volatile("mfence" ::: "memory");
        if (cur && cur->vmm_space) {
            __atomic_fetch_and(&cur->vmm_space->active_cpus, ~(1ULL << my_cpu), __ATOMIC_SEQ_CST);
        }
        wrmsr(MSR_FS_BASE, chosen->fs_base);
    }

    chosen->ticks++;
    uint64_t next_rsp = chosen->rsp;

    if (next_rsp != current_rsp) {
        fpu_switch(current_rsp, next_rsp);
    }

    spinlock_release_irqrestore(&runqueue_lock, rflags);
    return next_rsp;
}

static process_t *pid_table_find_unlocked(uint32_t pid) {
    uint32_t idx = pid_hash(pid);
    process_t *curr = pid_hash_table[idx];
    while (curr) {
        if (curr->pid == pid) {
            return curr;
        }
        curr = curr->pid_hash_next;
    }
    return NULL;
}

static void reparent_cb(process_t *proc, void *arg) {
    uint32_t parent_pid = *(uint32_t *)arg;
    if (proc && proc->parent_pid == parent_pid) {
        proc->parent_pid = reaper_pid;
        if (proc->state == PROC_STATE_ZOMBIE) {
            if (reaper_pid != 0) {
                process_t *reaper = pid_table_find_unlocked(reaper_pid);
                if (reaper) {
                    wait_queue_wake_all(&reaper->wait_exit_queue);
                }
            } else {
                pid_table_remove_unlocked(proc->pid);
                proc->reaped = true;
                process_put(proc);
            }
        }
    }
}

static void process_release_shm(process_t *proc) {
    for (uint32_t i = 0; i < proc->shm_mapping_count; i++) {
        if (proc->shm_mappings[i].seg) {
            if (proc->pml4_phys) {
                uint64_t addr = proc->shm_mappings[i].addr;
                uint64_t len = proc->shm_mappings[i].length;
                for (uint64_t off = 0; off < len; off += 4096) {
                    extern void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);
                    paging_unmap_page(proc->pml4_phys, addr + off);
                }
            }
            shm_unref((shm_segment_t *)proc->shm_mappings[i].seg);
            proc->shm_mappings[i].seg = NULL;
        }
    }
    proc->shm_mapping_count = 0;
}


static void process_cleanup_inner(process_t *proc) {
    if (!proc || proc->pid == 0) return;

    bool is_last_thread = true;
    if (proc->pml4_refcount && __atomic_load_n(proc->pml4_refcount, __ATOMIC_RELAXED) > 1) {
        is_last_thread = false;
    }

    if (is_last_thread) {
        for (uint32_t i = 0; i < proc->elf_segment_count; i++) {
            if (proc->elf_segments[i].ptr) {
                if (proc->pml4_phys) {
                    for (uint64_t off = 0; off < proc->elf_segments[i].size; off += 4096) {
                        extern void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);
                        paging_unmap_page(proc->pml4_phys, proc->elf_segments[i].vaddr + off);
                    }
                }
                kfree_null(proc->elf_segments[i].ptr);
                proc->elf_segments[i].ptr = NULL;
            }
        }
        proc->elf_segment_count = 0;

    }

    poll_cleanup(proc);

    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        process_close_fd_inner(proc, i);
    }

    proc->mmap_allocation_count = 0;

    if (is_last_thread) {
        process_release_shm(proc);
    }


    if (proc->is_terminal_proc && proc->tty_id >= 0) {
        extern void tty_set_blit_enabled_for_id(int id, bool enabled);
        tty_set_blit_enabled_for_id(proc->tty_id, true);
    }

    uint32_t pid_val = proc->pid;
    process_table_for_each(reparent_cb, &pid_val);

    extern void cmd_process_finished(void);
    cmd_process_finished();
}

#define MAX_TTY_KILL_PROCS 64
typedef struct {
    int tty_id;
    process_t *procs[MAX_TTY_KILL_PROCS];
    int count;
} kill_tty_arg_t;

static void kill_tty_cb(process_t *proc, void *arg) {
    kill_tty_arg_t *a = (kill_tty_arg_t *)arg;
    if (proc && proc->pid != 0 && proc->tty_id == a->tty_id) {
        if (proc->state != PROC_STATE_ZOMBIE && !proc->kill_pending) {
            if (a->count < MAX_TTY_KILL_PROCS) {
                process_hold(proc);
                a->procs[a->count++] = proc;
            }
        }
    }
}

static void find_child_tty_cb(process_t *proc, void *arg) {
    kill_tty_arg_t *a = (kill_tty_arg_t *)arg;
    if (proc && proc->pid > 1 && proc->tty_id == a->tty_id) {
        if (proc->state != PROC_STATE_ZOMBIE && !proc->kill_pending && !proc->is_terminal_proc) {
            if (!a->procs[0] || proc->pid > a->procs[0]->pid) {
                a->procs[0] = proc;
            }
        }
    }
}

process_t* process_find_child_on_tty(int tty_id) {
    kill_tty_arg_t a = { .tty_id = tty_id, .procs = {0}, .count = 0 };
    process_table_for_each(find_child_tty_cb, &a);
    return a.procs[0];
}

void process_kill_by_tty(int tty_id) {
    kill_tty_arg_t a = { .tty_id = tty_id, .procs = {0}, .count = 0 };
    process_table_for_each(kill_tty_cb, &a);
    for (int i = 0; i < a.count; i++) {
        process_terminate(a.procs[i]);
        process_put(a.procs[i]);
    }
}

void process_terminate(process_t *to_delete) {
    process_terminate_with_status(to_delete, 128 + 9);
}

void process_terminate_with_status(process_t *to_delete, int status) {
    if (!to_delete || to_delete->pid == 0) return;
    if (to_delete->state == PROC_STATE_ZOMBIE || to_delete->kill_pending) return;

    uint32_t cpu_count = smp_cpu_count();
    for (uint32_t c = 0; c < cpu_count && c < MAX_CPUS_SCHED; c++) {
        if (process_get_current_for_cpu(c) == to_delete) {
            to_delete->kill_pending = true;
            to_delete->exit_status = status;
            return;
        }
    }

    process_cleanup_inner(to_delete);

    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);

    to_delete->state = PROC_STATE_ZOMBIE;
    to_delete->exited = true;
    to_delete->exit_status = status;
    to_delete->kill_pending = false;

    process_t *prev = to_delete;
    while (prev->next && prev->next != to_delete) {
        prev = prev->next;
    }
    if (prev->next == to_delete) {
        prev->next = to_delete->next;
    }

    spinlock_release_irqrestore(&runqueue_lock, rflags);

    if (to_delete->parent_pid != 0) {
        process_t *parent = process_get_by_pid(to_delete->parent_pid);
        if (parent) {
            wait_queue_wake_all(&parent->wait_exit_queue);
            process_put(parent);
        }
    }
    if (reaper_pid != 0 && to_delete->parent_pid != reaper_pid) {
        process_t *reaper = process_get_by_pid(reaper_pid);
        if (reaper) {
            wait_queue_wake_all(&reaper->wait_exit_queue);
            process_put(reaper);
        }
    }

    if (to_delete->parent_pid == 0 && reaper_pid == 0) {
        pid_table_remove(to_delete->pid);
        to_delete->reaped = true;
        process_put(to_delete);
    }
}



uint64_t process_terminate_current_with_status(int status, uint64_t current_rsp) {
    process_t *cur = (process_t *)current_cpu->current_process;
    if (!cur || cur->pid == 0) {
        return current_rsp;
    }

    process_hold(cur);
    process_cleanup_inner(cur);

    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);
    uint32_t my_cpu = current_cpu->cpu_id;

    cur->exited = true;
    cur->exit_status = status;
    cur->state = PROC_STATE_ZOMBIE;
    cur->kill_pending = false;
    cur->running_cpu = -1;

    process_t *prev = cur;
    while (prev->next && prev->next != cur) {
        prev = prev->next;
    }
    if (prev->next == cur) {
        prev->next = cur->next;
    }

    process_t *next_proc = NULL;
    process_t *start = cur->next;
    process_t *scan = start;
    if (scan) {
        do {
            bool matches_cpu = (scan->cpu_affinity == my_cpu || scan->cpu_affinity == CPU_AFFINITY_ANY);
            bool not_running_elsewhere = (scan->running_cpu == -1 || scan->running_cpu == (int)my_cpu);
            bool is_runnable = (!scan->kill_pending && !scan->exited && scan->state == PROC_STATE_RUNNING);

            if (matches_cpu && not_running_elsewhere && is_runnable) {
                next_proc = scan;
                break;
            }
            scan = scan->next;
        } while (scan != start && scan != NULL);
    }

    if (!next_proc) {
        next_proc = (process_t *)current_cpu->idle_process;
    }

    if (!next_proc) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return current_rsp;
    }

    current_cpu->current_process = (struct process *)next_proc;
    next_proc->running_cpu = (int)my_cpu;

    if (next_proc->kernel_stack) {
        tss_set_stack_cpu(my_cpu, next_proc->kernel_stack);
        current_cpu->kernel_syscall_stack = next_proc->kernel_stack;
    }

    if (next_proc->vmm_space) {
        __atomic_fetch_or(&next_proc->vmm_space->active_cpus, (1ULL << my_cpu), __ATOMIC_SEQ_CST);
    }
    asm volatile("mfence" ::: "memory");
    paging_switch_directory(next_proc->pml4_phys);
    asm volatile("mfence" ::: "memory");
    if (cur && cur->vmm_space) {
        __atomic_fetch_and(&cur->vmm_space->active_cpus, ~(1ULL << my_cpu), __ATOMIC_SEQ_CST);
    }
    wrmsr(MSR_FS_BASE, next_proc->fs_base);


    if (cur->parent_pid != 0) {
        process_t *parent = pid_table_find_unlocked(cur->parent_pid);
        if (parent) {
            wait_queue_wake_all(&parent->wait_exit_queue);
        }
    }
    if (reaper_pid != 0 && cur->parent_pid != reaper_pid) {
        process_t *reaper = pid_table_find_unlocked(reaper_pid);
        if (reaper) {
            wait_queue_wake_all(&reaper->wait_exit_queue);
        }
    }

    if (cur->parent_pid == 0 && reaper_pid == 0) {
        pid_table_remove(cur->pid);
        cur->reaped = true;
        process_put(cur);
    }

    uint64_t next_rsp = next_proc->rsp;


    current_cpu->process_free_later = (struct process *)cur;

    if (next_rsp != current_rsp) {
        fpu_switch(current_rsp, next_rsp);
    }

    spinlock_release_irqrestore(&runqueue_lock, rflags);
    return next_rsp;
}

uint64_t process_terminate_current(uint64_t current_rsp) {
    return process_terminate_current_with_status(0, current_rsp);
}

int process_reap(uint32_t caller_pid, uint32_t pid, int *status_out) {
    process_t *p = process_get_by_pid(pid);
    if (!p) return -1;

    if (p->state != PROC_STATE_ZOMBIE) {
        process_put(p);
        return -2;
    }

    if (p->parent_pid != caller_pid && caller_pid != 0 && p->parent_pid != 0) {
        process_put(p);
        return -1;
    }

    if (status_out) {
        *status_out = p->exit_status;
    }

    p->reaped = true;
    pid_table_remove(pid);
    process_put(p);
    process_put(p);

    return 0;
}

typedef struct {
    uint32_t caller_pid;
    int target_pid;
    process_t *match_zombie;
    int child_count;
} waitpid_scan_arg_t;

static void waitpid_scan_cb(process_t *p, void *arg) {
    waitpid_scan_arg_t *w = (waitpid_scan_arg_t *)arg;
    if (!p || p->pid == 0 || p->parent_pid != w->caller_pid) return;

    bool match = false;
    if (w->target_pid > 0) match = ((int)p->pid == w->target_pid);
    else if (w->target_pid == -1) match = true;
    else if (w->target_pid == 0) {
        process_t *caller = pid_table_find_unlocked(w->caller_pid);
        if (caller) {
            match = (p->pgid == caller->pgid);
        }
    } else match = (p->pgid == (uint32_t)(-w->target_pid));

    if (match) {
        w->child_count++;
        if (p->state == PROC_STATE_ZOMBIE && !w->match_zombie) {
            w->match_zombie = p;
            process_hold(p);
        }
    }
}

int process_waitpid(uint32_t caller_pid, int target_pid, int options, int *status_out) {
    while (1) {
        waitpid_scan_arg_t w = { .caller_pid = caller_pid, .target_pid = target_pid, .match_zombie = NULL, .child_count = 0 };
        process_table_for_each(waitpid_scan_cb, &w);

        if (w.match_zombie) {
            process_t *z = w.match_zombie;
            uint32_t reaped_pid = z->pid;
            if (status_out) {
                *status_out = z->exit_status;
            }
            z->reaped = true;
            pid_table_remove(reaped_pid);
            process_put(z);
            process_put(z);
            return (int)reaped_pid;
        }

        if (w.child_count == 0) {
            return -1;
        }

        if (options & 1) { // WNOHANG
            return 0;
        }

        process_t *caller = process_get_current();
        if (caller) {
            wait_queue_entry_t entry = { .proc = caller, .next = NULL };
            wait_queue_add(&caller->wait_exit_queue, &entry);
            caller->state = PROC_STATE_BLOCKED;
            asm volatile("int $0x20");
            wait_queue_remove(&caller->wait_exit_queue, &entry);
        } else {
            return -1;
        }
    }
}

int process_exec_replace_current(registers_t *regs, const char* filepath, const char* args_str) {
    process_t *proc = process_get_current();
    if (!proc || !proc->is_user || !regs || !filepath) return -1;

    vmm_space_t *new_space = vmm_create_space();
    uint64_t new_pml4 = new_space ? new_space->mmu_ctx->pml4_phys : paging_create_user_pml4_phys();
    if (!new_pml4) {
        if (new_space) {
            vmm_destroy_space(new_space);
        }
        return -1;
    }

    for (uint32_t i = 0; i < proc->elf_segment_count; i++) {
        if (proc->elf_segments[i].ptr) {
            if (proc->pml4_phys) {
                for (uint64_t off = 0; off < proc->elf_segments[i].size; off += 4096) {
                    extern void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);
                    paging_unmap_page(proc->pml4_phys, proc->elf_segments[i].vaddr + off);
                }
            }
            kfree_null(proc->elf_segments[i].ptr);
            proc->elf_segments[i].ptr = NULL;
        }
    }
    proc->elf_segment_count = 0;


    vmm_space_t *saved_space = proc->vmm_space;
    proc->vmm_space = new_space;

    size_t elf_load_size = 0;
    uint64_t phdr_vaddr = 0, phdr_num = 0;
    uint64_t entry_point = elf_load(filepath, new_pml4, &elf_load_size, proc, &phdr_vaddr, &phdr_num);
    if (entry_point == 0) {
        proc->vmm_space = saved_space;
        if (new_space) {
            vmm_destroy_space(new_space);
        } else {
            extern void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped);
            paging_destroy_user_pml4_phys(new_pml4, true);
        }
        return -1;
    }

    size_t user_stack_size = 262144;
    void* stack = kmalloc_aligned(user_stack_size, 4096);
    if (!stack) {
        proc->vmm_space = saved_space;
        if (new_space) {
            vmm_destroy_space(new_space);
        } else {
            extern void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped);
            paging_destroy_user_pml4_phys(new_pml4, true);
        }
        return -1;
    }

    for (uint64_t i = 0; i < (user_stack_size / 4096); i++) {
        if (!paging_map_page(new_pml4, 0x800000 - user_stack_size + (i * 4096), v2p((uint64_t)stack + (i * 4096)), PT_PRESENT | PT_RW | PT_USER)) {
            kfree_null(stack);
            proc->vmm_space = saved_space;
            if (new_space) {
                vmm_destroy_space(new_space);
            } else {
                extern void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped);
                paging_destroy_user_pml4_phys(new_pml4, true);
            }
            return -1;
        }
    }
    proc->vmm_space = saved_space;

    int argc = 1;
    char *args_buf = (char *)stack + user_stack_size;
    uint64_t user_args_buf = 0x800000;

    int path_len = 0;
    while (filepath[path_len]) path_len++;
    args_buf -= (path_len + 1);
    user_args_buf -= (path_len + 1);
    for (int i = 0; i <= path_len; i++) args_buf[i] = filepath[i];

    uint64_t argv_ptrs[32];
    argv_ptrs[0] = user_args_buf;

    if (args_str) {
        int i = 0;
        while (args_str[i] && argc < 31) {
            while (args_str[i] == ' ') i++;
            if (!args_str[i]) break;

            int arg_start = i;
            bool in_quotes = false;
            if (args_str[i] == '"') {
                in_quotes = true;
                i++;
                arg_start = i;
                while (args_str[i] && args_str[i] != '"') i++;
            } else {
                while (args_str[i] && args_str[i] != ' ') i++;
            }

            int arg_len = i - arg_start;
            args_buf -= (arg_len + 1);
            user_args_buf -= (arg_len + 1);
            for (int k = 0; k < arg_len; k++) args_buf[k] = args_str[arg_start + k];
            args_buf[arg_len] = '\0';
            argv_ptrs[argc++] = user_args_buf;
            if (in_quotes && args_str[i] == '"') i++;
        }
    }
    argv_ptrs[argc] = 0;

    int total_elements = 1 + (argc + 1) + 1 + 14; 
    int total_size = total_elements * (int)sizeof(uint64_t);

    uint64_t target_sp = (user_args_buf - total_size) & ~15ULL;
    uint64_t current_user_sp = target_sp + total_size;
    
    args_buf = (char *)((uint64_t)stack + (current_user_sp - (0x800000 - user_stack_size)));

    // 1. Push AUXV (mlibc standard entries + AT_NULL terminator)
    args_buf -= 14 * sizeof(uint64_t);
    current_user_sp -= 14 * sizeof(uint64_t);
    uint64_t *user_auxv = (uint64_t *)args_buf;
    user_auxv[0] = 9;   user_auxv[1] = entry_point;    // AT_ENTRY
    user_auxv[2] = 6;   user_auxv[3] = 4096;           // AT_PAGESZ
    user_auxv[4] = 3;   user_auxv[5] = phdr_vaddr;     // AT_PHDR
    user_auxv[6] = 4;   user_auxv[7] = 56;             // AT_PHENT (sizeof(Elf64_Phdr))
    user_auxv[8] = 5;   user_auxv[9] = phdr_num;       // AT_PHNUM
    user_auxv[10] = 25; user_auxv[11] = user_args_buf; // AT_RANDOM
    user_auxv[12] = 0;  user_auxv[13] = 0;             // AT_NULL

    args_buf -= 1 * sizeof(uint64_t);
    current_user_sp -= 1 * sizeof(uint64_t);
    uint64_t *user_envp = (uint64_t *)args_buf;
    user_envp[0] = 0;

    int argv_size = (argc + 1) * sizeof(uint64_t);
    args_buf -= argv_size;
    current_user_sp -= argv_size;
    uint64_t *user_argv_array = (uint64_t *)args_buf;
    for (int i = 0; i <= argc; i++) {
        user_argv_array[i] = argv_ptrs[i];
    }
    uint64_t actual_argv_ptr = current_user_sp;

    args_buf -= 1 * sizeof(uint64_t);
    current_user_sp -= 1 * sizeof(uint64_t);
    uint64_t *user_argc = (uint64_t *)args_buf;
    user_argc[0] = (uint64_t)argc;
    vmm_space_t *old_space = proc->vmm_space;
    uint64_t old_pml4 = proc->pml4_phys;
    int *old_refcount = proc->pml4_refcount;
    void *old_stack = proc->user_stack_alloc;

    int *new_refcount = (int *)kmalloc(sizeof(int));
    if (new_refcount) *new_refcount = 1;

    proc->vmm_space = new_space;
    proc->pml4_phys = new_pml4;
    proc->pml4_refcount = new_refcount;
    proc->user_stack_alloc = stack;

    if (new_space) {
        vm_area_t *stack_vma = vma_create(0x800000 - user_stack_size, 0x800000, VMA_TYPE_ANON,
                                          VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_STACK);
        if (stack_vma) {
            vma_insert(&new_space->vma_head, &new_space->vma_tree, stack_vma);
        }
        new_space->brk_start = 0x20000000;
        new_space->brk_current = 0x20000000;
        vm_area_t *heap_vma = vma_create(0x20000000, 0x20000000, VMA_TYPE_ANON,
                                         VMA_FLAG_READ | VMA_FLAG_WRITE | VMA_FLAG_HEAP);
        if (heap_vma) {
            vma_insert(&new_space->vma_head, &new_space->vma_tree, heap_vma);
        }
    }

    paging_switch_directory(new_pml4);

    bool destroy_old = true;
    if (old_refcount) {
        int refs = __atomic_fetch_sub(old_refcount, 1, __ATOMIC_RELEASE);
        if (refs > 1) {
            destroy_old = false;
        } else {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            kfree_null(old_refcount);
        }
    }

    if (old_stack) {
        if (old_pml4) {
            extern void paging_unmap_page(uint64_t pml4_phys, uint64_t virtual_addr);
            uint64_t stack_top = 0x800000;
            for (uint64_t off = 0; off < user_stack_size; off += 4096) {
                paging_unmap_page(old_pml4, stack_top - user_stack_size + off);
            }
        }
        kfree_null(old_stack);
    }
    if (old_space && destroy_old) {
        vmm_destroy_space(old_space);
    } else if (old_pml4 && destroy_old) {
        extern void paging_destroy_user_pml4_phys(uint64_t pml4_phys, bool free_mapped);
        paging_destroy_user_pml4_phys(old_pml4, true);
    }

    proc->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    proc->is_cloned_child = false;
    proc->used_memory = elf_load_size + user_stack_size + KERNEL_STACK_SIZE;
    proc->heap_start = 0x20000000;
    proc->heap_end = 0x20000000;
    proc->mmap_current = 0x50000000;
    proc->mmap_allocation_count = 0;
    for (int i = 0; i < 16; i++) proc->mmap_allocations[i] = NULL;
    process_release_shm(proc);
    for (int i = 0; i < 32; i++) {
        proc->shm_mappings[i].addr = 0;
        proc->shm_mappings[i].length = 0;
        proc->shm_mappings[i].seg = NULL;
    }
    proc->sleep_until = 0;
    process_init_signal_state(proc);

    for (int i = 3; i < MAX_PROCESS_FDS; i++) {
        if (proc->fds[i]) {
            process_close_fd_inner(proc, i);
        }
        proc->fds[i] = NULL;
        proc->fd_kind[i] = PROC_FD_KIND_NONE;
        proc->fd_flags[i] = 0;
    }

    int last_slash = -1;
    for (int i = 0; filepath[i]; i++) if (filepath[i] == '/') last_slash = i;
    const char *filename = (last_slash == -1) ? filepath : (filepath + last_slash + 1);
    int ni = 0;
    while (filename[ni] && ni < 63) {
        proc->name[ni] = filename[ni];
        ni++;
    }
    proc->name[ni] = 0;

    regs->rip = entry_point;
    regs->rdi = argc;
    regs->rsi = actual_argv_ptr;
    regs->rsp = current_user_sp;
    regs->rax = 0;
    regs->rbx = 0;
    regs->rcx = 0;
    regs->rdx = 0;
    regs->r8 = 0;
    regs->r9 = 0;
    regs->r10 = 0;
    regs->r11 = 0;
    regs->r12 = 0;
    regs->r13 = 0;
    regs->r14 = 0;
    regs->r15 = 0;
    regs->rbp = 0;
    return 0;
}

uint64_t sched_ipi_handler(registers_t *regs) {
    lapic_eoi();
    return process_schedule((uint64_t)regs);
}

uint64_t tlb_shootdown_ipi_handler(registers_t *regs) {
    extern void mmu_tlb_ipi_handler(void);
    mmu_tlb_ipi_handler();
    lapic_eoi();
    return (uint64_t)regs;
}

process_t* process_duplicate(registers_t *parent_regs) {
    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);

    process_t *parent = process_get_current();
    if (!parent) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }

    process_t *child = process_alloc_struct();
    if (!child) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }

    child->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    child->refcount = 1;
    child->running_cpu = -1;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->is_user = parent->is_user;
    child->state = PROC_STATE_RUNNING;
    child->cpu_affinity = parent->cpu_affinity;
    child->exited = false;
    child->exit_status = 0;
    child->sleep_until = 0;
    child->ticks = 0;
    child->tty_id = parent->tty_id;
    child->is_terminal_proc = parent->is_terminal_proc;
    child->kill_pending = false;
    child->used_memory = parent->used_memory;
    child->is_cloned_child = true;
    child->fs_base = parent->fs_base;

    memcpy(child->cwd, parent->cwd, 1024);
    memcpy(child->name, parent->name, 64);
    int len = 0;
    while (child->name[len]) len++;
    if (len < 55) {
        child->name[len++] = '-';
        child->name[len++] = 'c';
        child->name[len++] = 'h';
        child->name[len++] = 'i';
        child->name[len++] = 'l';
        child->name[len++] = 'd';
        child->name[len] = 0;
    }

    if (parent->vmm_space) {
        child->vmm_space = vmm_clone_space(parent->vmm_space);
        if (child->vmm_space) {
            child->pml4_phys = child->vmm_space->mmu_ctx->pml4_phys;
        }
    } else {
        extern uint64_t paging_clone_user_pml4(uint64_t parent_pml4_phys);
        child->pml4_phys = paging_clone_user_pml4(parent->pml4_phys);
    }
    if (!child->pml4_phys) {
        process_put(child);
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }
    child->pml4_refcount = (int *)kmalloc(sizeof(int));
    if (child->pml4_refcount) *child->pml4_refcount = 1;

    size_t stack_size = (uint64_t)parent->kernel_stack - (uint64_t)parent->kernel_stack_alloc;
    if (stack_size == 0) stack_size = KERNEL_STACK_SIZE;

    const size_t stack_alignment = 4096;
    child->kernel_stack_alloc = kmalloc_aligned(stack_size, stack_alignment);
    if (!child->kernel_stack_alloc) {
        process_put(child);
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }
    child->kernel_stack = (uint64_t)child->kernel_stack_alloc + stack_size;
    child->user_stack_alloc = NULL;

    fpu_save_to(parent_regs->fxsave_region);

    child->rsp = child->kernel_stack - sizeof(registers_t);
    memcpy((void *)child->rsp, (const void *)parent_regs, sizeof(registers_t));

    registers_t *child_regs = (registers_t *)child->rsp;
    child_regs->rax = 0;

    child->fpu_initialized = parent->fpu_initialized;

    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        child->fds[i] = parent->fds[i];
        child->fd_kind[i] = parent->fd_kind[i];
        child->fd_flags[i] = parent->fd_flags[i];
        if (child->fds[i]) {
            if (child->fd_kind[i] == PROC_FD_KIND_FILE) {
                process_fd_file_ref_t *ref = (process_fd_file_ref_t *)child->fds[i];
                __atomic_fetch_add(&ref->refs, 1, __ATOMIC_RELAXED);
            } else if (child->fd_kind[i] == PROC_FD_KIND_PIPE_READ || child->fd_kind[i] == PROC_FD_KIND_PIPE_WRITE) {
                process_fd_pipe_t *pipe = (process_fd_pipe_t *)child->fds[i];
                if (child->fd_kind[i] == PROC_FD_KIND_PIPE_READ) __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELAXED);
                else __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELAXED);
            } else if (child->fd_kind[i] == PROC_FD_KIND_SOCKET) {
                process_socket_addref((process_fd_socket_t *)child->fds[i]);
            }
        }
    }

    child->elf_segment_count = 0;
    memset(child->elf_segments, 0, sizeof(child->elf_segments));
    child->mmap_allocation_count = 0;
    child->shm_mapping_count = parent->shm_mapping_count;
    for (uint32_t i = 0; i < parent->shm_mapping_count; i++) {
        child->shm_mappings[i] = parent->shm_mappings[i];
        if (child->shm_mappings[i].seg) {
            shm_ref((shm_segment_t *)child->shm_mappings[i].seg);
        }
    }
    for (uint32_t i = parent->shm_mapping_count; i < 32; i++) {
        child->shm_mappings[i].addr = 0;
        child->shm_mappings[i].length = 0;
        child->shm_mappings[i].seg = NULL;
    }
    child->mmap_current = parent->mmap_current;

    child->next = parent->next;
    parent->next = child;

    spinlock_release_irqrestore(&runqueue_lock, rflags);
    pid_table_insert(child);

    return child;
}

process_t* process_create_thread(registers_t *parent_regs, uint64_t entry_point, uint64_t user_sp, uint64_t flags) {
    (void)flags;
    uint64_t rflags = spinlock_acquire_irqsave(&runqueue_lock);

    process_t *parent = process_get_current();
    if (!parent) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }

    process_t *child = process_alloc_struct();
    if (!child) {
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }

    child->pid = __atomic_fetch_add(&next_pid, 1, __ATOMIC_SEQ_CST);
    child->tgid = parent->tgid ? parent->tgid : parent->pid;
    child->is_thread = true;
    child->refcount = 1;
    child->running_cpu = -1;
    child->parent_pid = parent->pid;
    child->pgid = parent->pgid;
    child->is_user = parent->is_user;
    child->state = PROC_STATE_RUNNING;
    child->cpu_affinity = parent->cpu_affinity;
    child->exited = false;
    child->exit_status = 0;
    child->sleep_until = 0;
    child->ticks = 0;
    child->tty_id = parent->tty_id;
    child->is_terminal_proc = parent->is_terminal_proc;
    child->kill_pending = false;
    child->used_memory = parent->used_memory;
    child->is_cloned_child = true;
    child->fs_base = parent->fs_base;
    child->heap_start = parent->heap_start;
    child->heap_end = parent->heap_end;

    memcpy(child->cwd, parent->cwd, 1024);
    int len = 0;
    while (parent->name[len] && len < 55) {
        child->name[len] = parent->name[len];
        len++;
    }
    child->name[len++] = '-';
    child->name[len++] = 't';
    child->name[len++] = 'h';
    child->name[len++] = 'd';
    child->name[len] = 0;
    child->vmm_space = parent->vmm_space;
    child->mmap_current = parent->mmap_current;
    child->pml4_phys = parent->pml4_phys;
    child->pml4_refcount = parent->pml4_refcount;
    if (child->pml4_refcount) {
        __atomic_fetch_add(child->pml4_refcount, 1, __ATOMIC_RELAXED);
    }

    const size_t stack_alignment = 4096;
    child->kernel_stack_alloc = kmalloc_aligned(KERNEL_STACK_SIZE, stack_alignment);
    if (!child->kernel_stack_alloc) {
        if (child->pml4_refcount) {
            __atomic_fetch_sub(child->pml4_refcount, 1, __ATOMIC_RELAXED);
        }
        kfree_null(child);
        spinlock_release_irqrestore(&runqueue_lock, rflags);
        return NULL;
    }
    memset(child->kernel_stack_alloc, 0, KERNEL_STACK_SIZE);
    child->kernel_stack = (uint64_t)child->kernel_stack_alloc + KERNEL_STACK_SIZE;
    child->user_stack_alloc = NULL;

    fpu_save_to(parent_regs->fxsave_region);
    child->fpu_initialized = parent->fpu_initialized;

    child->rsp = child->kernel_stack - sizeof(registers_t);
    registers_t *child_regs = (registers_t *)child->rsp;
    memset(child_regs, 0, sizeof(registers_t));
    memcpy(child_regs->fxsave_region, parent_regs->fxsave_region, 512);

    child_regs->rip = entry_point;
    child_regs->rsp = user_sp;
    child_regs->cs = 0x23;
    child_regs->ss = 0x1B;
    child_regs->rflags = 0x202;
    child_regs->rax = 0;

    for (int i = 0; i < MAX_PROCESS_FDS; i++) {
        child->fds[i] = parent->fds[i];
        child->fd_kind[i] = parent->fd_kind[i];
        child->fd_flags[i] = parent->fd_flags[i];
        if (child->fds[i]) {
            if (child->fd_kind[i] == PROC_FD_KIND_FILE) {
                process_fd_file_ref_t *ref = (process_fd_file_ref_t *)child->fds[i];
                __atomic_fetch_add(&ref->refs, 1, __ATOMIC_RELAXED);
            } else if (child->fd_kind[i] == PROC_FD_KIND_PIPE_READ || child->fd_kind[i] == PROC_FD_KIND_PIPE_WRITE) {
                process_fd_pipe_t *pipe = (process_fd_pipe_t *)child->fds[i];
                if (child->fd_kind[i] == PROC_FD_KIND_PIPE_READ) __atomic_fetch_add(&pipe->readers, 1, __ATOMIC_RELAXED);
                else __atomic_fetch_add(&pipe->writers, 1, __ATOMIC_RELAXED);
            } else if (child->fd_kind[i] == PROC_FD_KIND_SOCKET) {
                process_socket_addref((process_fd_socket_t *)child->fds[i]);
            }
        }
    }

    child->elf_segment_count = parent->elf_segment_count;
    memcpy(child->elf_segments, parent->elf_segments, sizeof(parent->elf_segments));
    child->mmap_allocation_count = parent->mmap_allocation_count;
    child->shm_mapping_count = parent->shm_mapping_count;
    memcpy(child->shm_mappings, parent->shm_mappings, sizeof(parent->shm_mappings));

    child->next = parent->next;
    parent->next = child;

    spinlock_release_irqrestore(&runqueue_lock, rflags);
    pid_table_insert(child);

    return child;
}
