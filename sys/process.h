// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sockbuf.h"

#define MAX_PROCESS_FDS 64
#define MAX_SIGNALS 64

#define PROC_STATE_RUNNING 0
#define PROC_STATE_BLOCKED 1
#define PROC_STATE_ZOMBIE  2

#define CPU_AFFINITY_ANY   0xFFFFFFFF

#define PROC_FD_KIND_NONE 0
#define PROC_FD_KIND_FILE 1
#define PROC_FD_KIND_PIPE_READ 2
#define PROC_FD_KIND_PIPE_WRITE 3
#define PROC_FD_KIND_TTY 4
#define PROC_FD_KIND_SOCKET 5

// Socket Domains
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef AF_PACKET
#define AF_PACKET 17
#endif

// Socket Types
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#ifndef SOCK_DGRAM
#define SOCK_DGRAM 2
#endif
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif

// Socket Protocols
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6 58
#endif

// Signal numbers and bitmasks
#define SIGINT_CODE 2
#define SIGINT      (1ULL << SIGINT_CODE)

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
    spinlock_t lock;
    wait_queue_head_t read_queue;
    wait_queue_head_t write_queue;
} process_fd_pipe_t;

typedef struct accept_queue_entry {
    void *client_sock; // process_fd_socket_t*
    struct accept_queue_entry *next;
} accept_queue_entry_t;

typedef struct {
    int refs;
    spinlock_t lock;
    uint8_t domain;   // AF_UNIX, AF_INET, AF_INET6, AF_PACKET
    uint8_t type;     // SOCK_STREAM, SOCK_DGRAM, SOCK_RAW
    uint8_t protocol; // IPPROTO_ICMP, IPPROTO_ICMPV6, etc.
    uint8_t is_bound;
    uint8_t is_listening;
    uint8_t is_connected;
    char path[108];

    // Unix domain socket PCB
    void *unpcb;

    // TCP/IP socket fields
    void *pcb;              // Pointer to struct tcp_pcb or udp_pcb or raw_pcb
    sockbuf_t rx_sb;        // Receive socket buffer
    sockbuf_t tx_sb;        // Send socket buffer
    void *recv_queue;       // Pointer to struct pbuf
    uint8_t tcp_closed;
    uint8_t tcp_connect_error;
    uint8_t tcp_connect_done;

    // Socket options
    uint32_t rcvtimeo;      // Receive timeout in ms
    uint32_t sndtimeo;      // Send timeout in ms
    uint8_t  reuseaddr;
    uint8_t  reuseport;
    uint8_t  keepalive;
    uint8_t  nodelay;

    // Backlog of accepted client sockets
    uint32_t backlog_max;
    accept_queue_entry_t *accept_head;
    accept_queue_entry_t *accept_tail;
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

typedef struct {
    void *ptr;
    uint64_t vaddr;
    size_t size;
} elf_segment_info_t;

typedef struct process {
    uint32_t pid;
    uint32_t refcount;
    int running_cpu;
    bool reaped;
    wait_queue_head_t wait_exit_queue;
    uint64_t rsp; 
    uint64_t fs_base;
    uint64_t pml4_phys; 
    uint64_t kernel_stack; 
    bool is_user;
    int state;
    
    uint64_t heap_start;
    uint64_t heap_end;
    struct vmm_space *vmm_space;
    
    void *fds[MAX_PROCESS_FDS];
    uint8_t fd_kind[MAX_PROCESS_FDS];
    int fd_flags[MAX_PROCESS_FDS];
    
    void *kernel_stack_alloc; 
    void *user_stack_alloc;  

    bool is_terminal_proc;   
    int tty_id;              
    bool kill_pending;       

    struct process *next;
    struct process *pid_hash_next;

    bool fpu_initialized; 
    
    char name[64];
    uint64_t ticks;
    uint64_t sleep_until;
    size_t used_memory;
    uint32_t cpu_affinity;    
    bool is_idle;            
    char cwd[1024];          
    bool is_cloned_child;
    bool is_thread;
    uint32_t tgid;
    int *pml4_refcount;

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
    elf_segment_info_t elf_segments[16];
    uint32_t elf_segment_count;

    // Tracking for mmap memory allocations to allow full reclamation on exit.
    uint64_t mmap_current;
    void *mmap_allocations[16];
    uint32_t mmap_allocation_count;

    // Note: Heap memory in [heap_start, heap_end) is cleanly reclaimed on process exit via page table walking.

    // Tracking for shm mappings to allow full reclamation on exit.
    struct {
        uint64_t addr;
        uint64_t length;
        void *seg;
    } shm_mappings[64];
    uint32_t shm_mapping_count;

    struct futex_waiter_entry {
        uint32_t *uaddr;
        uint64_t pml4_phys;
        uintptr_t phys_addr;
        struct process *proc;
        struct futex_waiter_entry *next;
    } futex_waiter;
    char ping_result[64];
    poll_wtable_t poll_table;
    wait_queue_entry_t pipe_wait_entry;
    wait_queue_entry_t wait_node;
} __attribute__((aligned(16))) process_t;

// Loads the ELF executable at 'path' into the pagemap given by user_pml4.
// If 'proc' is provided, the physical segments are tracked for later reclamation.
// Returns true on success, or false on failure.
#include "elf.h"
bool elf_load(const char *path, uint64_t user_pml4, struct process *proc, elf_load_result_t *out_result);

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
process_t* process_create_thread(registers_t *parent_regs, uint64_t entry_point, uint64_t user_sp, uint64_t flags);
int process_exec_replace_current(registers_t *regs, const char* filepath, const char* args_str);
void process_close_fd_inner(process_t *proc, int fd);
process_t* process_get_current(void);
uint32_t   process_get_current_pid(void);
void process_set_current_for_cpu(uint32_t cpu_id, process_t* p);
process_t* process_get_current_for_cpu(uint32_t cpu_id);
void process_set_idle_for_cpu(uint32_t cpu_id, process_t* p);
process_t* process_get_idle_for_cpu(uint32_t cpu_id);
uint64_t process_schedule(uint64_t current_rsp);
uint64_t process_terminate_current(uint64_t current_rsp);
uint64_t process_terminate_current_with_status(int status, uint64_t current_rsp);
// Records an allocated ELF segment pointer so it can be freed when the process exits.
void process_add_elf_segment(struct process *proc, void *ptr, uint64_t vaddr, size_t size);

void process_terminate(process_t *proc);
void process_terminate_with_status(process_t *proc, int status);
process_t* process_get_by_pid(uint32_t pid);
void process_hold(process_t *proc);
void process_put(process_t *proc);
void process_table_for_each(void (*cb)(process_t *proc, void *arg), void *arg);
process_t* process_find_child_on_tty(int tty_id);
int process_get_all_pids(uint32_t *pids_out, int max_pids);
extern uint32_t reaper_pid; /* PID of the userspace zombie reaper daemon */
int process_waitpid(uint32_t caller_pid, int target_pid, int options, int *status_out);
int process_reap(uint32_t caller_pid, uint32_t pid, int *status_out);
void process_kill_by_tty(int tty_id);

// SMP: IPI handler for AP scheduling 
uint64_t sched_ipi_handler(registers_t *regs);

#endif

