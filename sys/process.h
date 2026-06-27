// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MAX_PROCESS_FDS 64
#define MAX_SIGNALS 32

#define PROC_STATE_RUNNING 0
#define PROC_STATE_BLOCKED 1

#define PROC_FD_KIND_NONE 0
#define PROC_FD_KIND_FILE 1
#define PROC_FD_KIND_PIPE_READ 2
#define PROC_FD_KIND_PIPE_WRITE 3
#define PROC_FD_KIND_TTY 4
#define PROC_FD_KIND_SOCKET 5

typedef struct {
    void *file;
    int refs;
} process_fd_file_ref_t;

#include "wait_queue.h"

typedef struct {
    uint8_t data[4096];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int readers;
    int writers;
    wait_queue_head_t read_queue;
    wait_queue_head_t write_queue;
} process_fd_pipe_t;

typedef struct {
    int refs;
    uint8_t domain; // 1 = AF_UNIX, 2 = AF_INET
    uint8_t is_bound;
    uint8_t is_listening;
    uint8_t is_connected;
    char path[108];
    process_fd_pipe_t *rx_pipe;
    process_fd_pipe_t *tx_pipe;

    // TCP/IP socket fields
    void *pcb;              // Pointer to struct tcp_pcb
    void *recv_queue;       // Pointer to struct pbuf
    uint8_t tcp_closed;
    uint8_t tcp_connect_error;
    uint8_t tcp_connect_done;

    // Backlog of accepted client sockets
    void *accept_queue[16]; // Backlog of process_fd_socket_t*
    int accept_queue_count;
    wait_queue_head_t accept_waitq;
    wait_queue_head_t rx_waitq;
} process_fd_socket_t;

process_fd_socket_t *process_socket_create(void);
void process_socket_addref(process_fd_socket_t *sock);
void process_socket_release(process_fd_socket_t *sock);

struct FAT32_FileHandle;

typedef struct registers_t {
    uint8_t fxsave_region[512]; 
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed, aligned(16))) registers_t;

typedef struct process {
    uint32_t pid;
    uint64_t rsp; 
    uint64_t fs_base;
    uint64_t pml4_phys; 
    uint64_t kernel_stack; 
    bool is_user;
    int state;
    
    uint64_t heap_start;
    uint64_t heap_end;
    
    void *fds[MAX_PROCESS_FDS];
    uint8_t fd_kind[MAX_PROCESS_FDS];
    int fd_flags[MAX_PROCESS_FDS];
    
    void *kernel_stack_alloc; 
    void *user_stack_alloc;  

    bool is_terminal_proc;   
    int tty_id;              
    bool kill_pending;       

    struct process *next;

    bool fpu_initialized; 
    
    char name[64];
    uint64_t ticks;
    uint64_t sleep_until;
    size_t used_memory;
    uint32_t cpu_affinity;    
    bool is_idle;            
    char cwd[1024];          
    bool is_cloned_child;

    uint32_t parent_pid;
    uint32_t pgid;
    bool exited;
    int exit_status;

    uint64_t signal_mask;
    uint64_t signal_pending;
    uint64_t signal_handlers[MAX_SIGNALS];
    uint64_t signal_action_mask[MAX_SIGNALS];
    int signal_action_flags[MAX_SIGNALS];
    
    // Tracking for ELF executable segments to allow full memory reclamation on exit.
    void *elf_segments[4];
    uint32_t elf_segment_count;

    // Tracking for mmap memory allocations to allow full reclamation on exit.
    uint64_t mmap_current;
    void *mmap_allocations[16];
    uint32_t mmap_allocation_count;

    // Tracking for sbrk heap allocations to allow full reclamation on exit.
    void *sbrk_allocations[64];
    uint32_t sbrk_allocation_count;

    // Tracking for shm mappings to allow full reclamation on exit.
    struct {
        uint64_t addr;
        uint64_t length;
        void *seg;
    } shm_mappings[32];
    uint32_t shm_mapping_count;

    poll_wtable_t poll_table;
} __attribute__((aligned(16))) process_t;

// Loads the ELF executable at 'path' using fat32 into the pagemap given by user_pml4.
// If 'proc' is provided, the physical segments are tracked for later reclamation.
// Returns entry point address on success, or 0 on failure.
struct process;
uint64_t elf_load(const char *path, uint64_t user_pml4, size_t *out_load_size, struct process *proc);

typedef struct {
    uint32_t pid;
    char name[64];
    uint64_t ticks;
    size_t used_memory;
    bool is_idle;
} ProcessInfo;

void process_init(void);
process_t* process_create(void (*entry_point)(void), bool is_user);
process_t* process_create_elf(const char* filepath, const char* args_str, bool terminal_proc, int tty_id);
int process_exec_replace_current(registers_t *regs, const char* filepath, const char* args_str);
process_t* process_get_current(void);
uint32_t   process_get_current_pid(void);
void process_set_current_for_cpu(uint32_t cpu_id, process_t* p);
process_t* process_get_current_for_cpu(uint32_t cpu_id);
uint64_t process_schedule(uint64_t current_rsp);
uint64_t process_terminate_current(uint64_t current_rsp);
uint64_t process_terminate_current_with_status(int status, uint64_t current_rsp);
// Records an allocated ELF segment pointer so it can be freed when the process exits.
void process_add_elf_segment(struct process *proc, void *ptr);

void process_terminate(process_t *proc);
void process_terminate_with_status(process_t *proc, int status);
process_t* process_get_by_pid(uint32_t pid);
int process_waitpid(uint32_t caller_pid, int target_pid, int options, int *status_out);
int process_reap(uint32_t caller_pid, uint32_t pid, int *status_out);
void process_kill_by_tty(int tty_id);

// SMP: IPI handler for AP scheduling 
uint64_t sched_ipi_handler(registers_t *regs);

#endif

