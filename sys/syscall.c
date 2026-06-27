// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See
// LICENSE file for details. This header needs to maintain in any file it is
// present in, as per the GPL license terms.
#include "syscall.h"
#include "gdt.h"
#include "memory_manager.h"
#include "process.h"
#include "vfs.h"
#include "shm.h"

#include "app_metadata.h"
#include "cmd.h"
#include "disk.h"
#include "fat32.h"
#include "graphics.h"
#include "icmp.h"
#include "keycodes.h"
#include "keymap.h"
#include "io.h"
#include "kutils.h"
#include "mkfs_fat32.h"
#include "network.h"
#include "paging.h"
#include "pci.h"
#include "platform.h"
#include "smp.h"
#include "tty.h"
#include "pty.h"
#include "unix_socket.h"
#include "vfs.h"
#include "wait_queue.h"
#include "work_queue.h"

#define SYSTEM_CMD_DISK_GET_COUNT 100
#define SYSTEM_CMD_DISK_GET_INFO 101
#define SYSTEM_CMD_DISK_WRITE_GPT 102
#define SYSTEM_CMD_DISK_WRITE_MBR 103
#define SYSTEM_CMD_DISK_MKFS_FAT32 104
#define SYSTEM_CMD_DISK_MOUNT 105
#define SYSTEM_CMD_DISK_UMOUNT 106
#define SYSTEM_CMD_DISK_RESCAN 107
#define SYSTEM_CMD_DISK_REPLACE_KERNEL 108
#define SYSTEM_CMD_DISK_SYNC 109

#define SPAWN_FLAG_TERMINAL 0x1
#define SPAWN_FLAG_INHERIT_TTY 0x2
#define SPAWN_FLAG_TTY_ID 0x4

#define SYSTEM_CMD_SET_KEYBOARD_LAYOUT 49
#define SYSTEM_CMD_GET_KEYBOARD_LAYOUT 51
#define SYSTEM_CMD_SET_FS_BASE 79
#define MSR_FS_BASE 0xC0000100

// Read MSR
static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value) {
  uint32_t low = value & 0xFFFFFFFF;
  uint32_t high = value >> 32;
  asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

extern void isr128_wrapper(void);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

typedef struct {
  void (*fn)(void *);
  void *arg;
  uint64_t pml4_phys;
  volatile int *completion_counter;
} smp_user_task_t;

static void smp_user_wrapper(void *arg) {
  smp_user_task_t *task = (smp_user_task_t *)arg;
  if (!task)
    return;

  uint64_t old_cr3;
  asm volatile("mov %%cr3, %0" : "=r"(old_cr3));

  // Switch to user address space if necessary
  bool switch_cr3 = (task->pml4_phys != 0 && task->pml4_phys != old_cr3);
  if (switch_cr3) {
    asm volatile("mov %0, %%cr3" ::"r"(task->pml4_phys) : "memory");
  }

  if (task->fn) {
    task->fn(task->arg);
  }

  if (switch_cr3) {
    asm volatile("mov %0, %%cr3" ::"r"(old_cr3) : "memory");
  }

  if (task->completion_counter) {
    __sync_fetch_and_add(task->completion_counter, -1);
  }
}

void syscall_init(void) {
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= 1;
  wrmsr(MSR_EFER, efer);
  uint64_t star = ((uint64_t)0x0013 << 48) | ((uint64_t)0x0008 << 32);
  wrmsr(MSR_STAR, star);
  extern void syscall_entry(void);
  wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
  wrmsr(MSR_FMASK, 0x200);
}

typedef struct {
  registers_t *regs;
  uint64_t arg1;
  uint64_t arg2;
  uint64_t arg3;
  uint64_t arg4;
  uint64_t arg5;
  uint64_t arg6;
} syscall_args_t;

typedef uint64_t (*syscall_handler_fn)(const syscall_args_t *args);

static process_fd_pipe_t *fs_create_pipe_state(void);
static uint64_t sys_cmd_get_pid(const syscall_args_t *args);
static void fs_pipe_drop_reader(process_fd_pipe_t *pipe);
static void fs_pipe_drop_writer(process_fd_pipe_t *pipe);
static int fs_copy_unix_path(const void *addr, uint64_t addrlen, char *path_out,
                             size_t path_out_size);
static uint64_t fs_cmd_unix_socket_create(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_bind(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_listen(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_accept(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_connect(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_close(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_unlink(const syscall_args_t *args);

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800
#define F_GETFL 3
#define F_SETFL 4

static int fs_alloc_fd_slot(process_t *proc, int start) {
  for (int i = start; i < MAX_PROCESS_FDS; i++) {
    if (!proc->fds[i])
      return i;
  }
  return -1;
}

static int fs_mode_to_flags(const char *mode) {
  if (!mode || !mode[0])
    return O_RDONLY;
  if (mode[0] == 'r') {
    return (mode[1] == '+') ? O_RDWR : O_RDONLY;
  }
  if (mode[0] == 'a') {
    return (mode[1] == '+') ? (O_RDWR | O_APPEND) : (O_WRONLY | O_APPEND);
  }
  if (mode[0] == 'w') {
    return (mode[1] == '+') ? O_RDWR : O_WRONLY;
  }
  return O_RDONLY;
}

static process_fd_pipe_t *fs_create_pipe_state(void) {
  process_fd_pipe_t *pipe =
      (process_fd_pipe_t *)kmalloc(sizeof(process_fd_pipe_t));
  if (!pipe)
    return NULL;
  memset(pipe, 0, sizeof(*pipe));
  pipe->readers = 1;
  pipe->writers = 1;
  wait_queue_init(&pipe->read_queue);
  wait_queue_init(&pipe->write_queue);
  return pipe;
}

static void fs_pipe_drop_reader(process_fd_pipe_t *pipe) {
  if (!pipe)
    return;
  pipe->readers--;
  if (pipe->readers <= 0 && pipe->writers <= 0) {
    kfree(pipe);
  }
}

static void fs_pipe_drop_writer(process_fd_pipe_t *pipe) {
  if (!pipe)
    return;
  pipe->writers--;
  if (pipe->readers <= 0 && pipe->writers <= 0) {
    kfree(pipe);
  }
}

static int fs_copy_unix_path(const void *addr, uint64_t addrlen, char *path_out,
                             size_t path_out_size) {
  extern void serial_write(const char *str);
  extern void serial_write_num(uint64_t n);

  const uint8_t *raw = (const uint8_t *)addr;
  size_t i;

  if (!addr || !path_out || path_out_size == 0 || addrlen < sizeof(uint16_t)) {
    serial_write("[fs_copy_unix_path] invalid arguments or short addrlen\n");
    return -1;
  }
  uint16_t family = *(const uint16_t *)addr;
  if (family != 1) {
    serial_write("[fs_copy_unix_path] family != 1 (AF_UNIX), family=");
    serial_write_num(family);
    serial_write("\n");
    return -1;
  }

  raw += sizeof(uint16_t);
  size_t offset = 0;
  if (addrlen > sizeof(uint16_t) && raw[0] == '\0') {
    offset = 1;
  }

  size_t limit = (offset == 1) ? 108 : (addrlen - sizeof(uint16_t));

  for (i = 0; i + 1 < path_out_size && i < limit; i++) {
    path_out[i] = (char)raw[i + offset];
    if (path_out[i] == '\0')
      break;
  }
  path_out[i] = '\0';
  return path_out[0] ? 0 : -1;
}

static void fs_socket_put_pipes(process_fd_socket_t *sock) {
  if (!sock)
    return;
  if (sock->rx_pipe) {
    fs_pipe_drop_reader(sock->rx_pipe);
    sock->rx_pipe = NULL;
  }
  if (sock->tx_pipe) {
    fs_pipe_drop_writer(sock->tx_pipe);
    sock->tx_pipe = NULL;
  }
  sock->is_connected = 0;
}

static uint64_t fs_cmd_unix_socket_create(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int domain = (int)args->arg2;
  int type = (int)args->arg3;
  int protocol = (int)args->arg4;

  if (!proc || (domain != 1 && domain != 2) || type != 1 || protocol != 0)
    return -1;

  int fd = fs_alloc_fd_slot(proc, 0);
  if (fd < 0)
    return -1;

  process_fd_socket_t *sock = process_socket_create();
  if (!sock)
    return -1;

  sock->domain = domain;
  if (domain == 2) {
    extern int network_is_initialized(void);
    extern int network_init(void);
    extern void serial_write(const char *str);
    if (!network_is_initialized()) {
      serial_write("[syscall] socket: domain=2 and network not initialized, "
                   "initializing network stack...\n");
      network_init();
    }
    sock->pcb = NULL;
    sock->recv_queue = NULL;
    sock->tcp_closed = 0;
    sock->tcp_connect_error = 0;
    sock->tcp_connect_done = 0;
    sock->accept_queue_count = 0;
    wait_queue_init(&sock->accept_waitq);
    wait_queue_init(&sock->rx_waitq);
  }

  proc->fds[fd] = sock;
  proc->fd_kind[fd] = PROC_FD_KIND_SOCKET;
  proc->fd_flags[fd] = O_RDWR;
  return fd;
}

static uint64_t fs_cmd_unix_socket_bind(const syscall_args_t *args) {
  extern void serial_write(const char *str);
  extern void serial_write_num(uint64_t n);
  serial_write("[syscall] bind called\n");

  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  const void *addr = (const void *)args->arg3;
  uint64_t addrlen = args->arg4;

  serial_write("[syscall] bind: fd=");
  serial_write_num(fd);
  serial_write(" addrlen=");
  serial_write_num(addrlen);
  serial_write("\n");

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET) {
    serial_write("[syscall] bind: invalid fd or proc check failed\n");
    return -1;
  }
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) {
    serial_write("[syscall] bind: sock is NULL\n");
    return -1;
  }

  serial_write("[syscall] bind: domain=");
  serial_write_num(sock->domain);
  serial_write("\n");

  if (sock->domain == 2) {
    if (addrlen < 8) {
      serial_write("[syscall] bind: addrlen < 8\n");
      return -1;
    }
    uint16_t family = *(const uint16_t *)addr;
    serial_write("[syscall] bind: family=");
    serial_write_num(family);
    serial_write("\n");

    if (family != 2) {
      serial_write("[syscall] bind: family != 2\n");
      return -1; // Must be AF_INET
    }

    uint16_t sin_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin_port & 0xFF) << 8) | ((sin_port >> 8) & 0xFF);
    uint32_t ip_val = *(const uint32_t *)((const char *)addr + 4);

    serial_write("[syscall] bind: port=");
    serial_write_num(port);
    serial_write(" ip_val=");
    serial_write_num(ip_val);
    serial_write("\n");

    int bind_err = network_socket_bind(sock, ip_val, port);
    serial_write("[syscall] bind: network_socket_bind returned ");
    if (bind_err < 0) {
      serial_write("-");
      serial_write_num(-bind_err);
    } else {
      serial_write_num(bind_err);
    }
    serial_write("\n");

    if (bind_err < 0)
      return bind_err;
    sock->is_bound = 1;
    sock->is_listening = 0;
    sock->is_connected = 0;
    return 0;
  } else {
    char path[108];
    if (fs_copy_unix_path(addr, addrlen, path, sizeof(path)) < 0)
      return -1;
    if (unix_register_listener(path, proc->pid, fd) < 0)
      return -1;
    serial_write("[bind-unix] path=");
    serial_write(path);
    serial_write(" registered listener\n");

    sock->is_bound = 1;
    sock->is_listening = 0;
    sock->is_connected = 0;
    strncpy(sock->path, path, sizeof(sock->path) - 1);
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_listen(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock || !sock->is_bound)
    return -1;

  if (sock->domain == 2) {
    if (network_socket_listen(sock) < 0)
      return -1;
    sock->is_listening = 1;
    return 0;
  } else {
    unix_listener_t *lst = unix_find_listener(sock->path);
    if (!lst)
      return -1;

    unix_listener_set_listening(lst, 1);
    sock->is_listening = 1;
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_connect(const syscall_args_t *args) {
  extern void serial_write(const char *str);
  extern void serial_write_num(uint64_t n);

  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  const void *addr = (const void *)args->arg3;
  uint64_t addrlen = args->arg4;
  process_fd_socket_t *sock;

  serial_write("[syscall] connect called: fd=");
  serial_write_num(fd);
  serial_write(" addrlen=");
  serial_write_num(addrlen);
  serial_write("\n");

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET) {
    serial_write("[syscall] connect: invalid fd or socket check failed\n");
    return -1;
  }
  sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) {
    serial_write("[syscall] connect: sock is NULL\n");
    return -1;
  }
  if (sock->is_connected) {
    serial_write("[syscall] connect: socket is already connected\n");
    return -1;
  }

  serial_write("[syscall] connect: domain=");
  serial_write_num(sock->domain);
  serial_write("\n");

  if (sock->domain == 2) {
    if (addrlen < 8) {
      serial_write("[syscall] connect: inet addrlen < 8\n");
      return -1;
    }
    uint16_t family = *(const uint16_t *)addr;
    if (family != 2) {
      serial_write("[syscall] connect: inet family != 2\n");
      return -1; // Must be AF_INET
    }

    uint16_t sin_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin_port & 0xFF) << 8) | ((sin_port >> 8) & 0xFF);
    uint32_t ip_val = *(const uint32_t *)((const char *)addr + 4);

    serial_write("[syscall] connect: inet connecting to port=");
    serial_write_num(port);
    serial_write("\n");

    if (network_socket_connect(sock, ip_val, port) < 0) {
      serial_write("[syscall] connect: network_socket_connect failed\n");
      return -1;
    }
    sock->is_connected = 1;
    serial_write("[syscall] connect: network_socket_connect succeeded\n");
    return 0;
  } else {
    char path[108];
    unix_listener_t *lst;
    process_fd_pipe_t *c2s;
    process_fd_pipe_t *s2c;
    unix_pending_conn_t *pc;

    if (fs_copy_unix_path(addr, addrlen, path, sizeof(path)) < 0) {
      serial_write("[syscall] connect: fs_copy_unix_path failed\n");
      return -1;
    }

    serial_write("[syscall] connect: unix connecting to path='");
    serial_write(path);
    serial_write("'\n");

    lst = unix_find_listener(path);
    serial_write("[connect-unix] path=");
    serial_write(path);
    serial_write(" lst=");
    serial_write_num((uint64_t)lst);
    serial_write("\n");
    if (!lst) {
      serial_write("[syscall] connect: unix listener not found for path\n");
      return -1;
    }
    if (!unix_listener_is_listening(lst)) {
      serial_write("[syscall] connect: unix listener is not listening\n");
      return -1;
    }

    c2s = fs_create_pipe_state();
    s2c = fs_create_pipe_state();
    if (!c2s || !s2c) {
      serial_write("[syscall] connect: failed to create pipe states\n");
      if (c2s)
        kfree(c2s);
      if (s2c)
        kfree(s2c);
      return -1;
    }

    sock->rx_pipe = s2c;
    sock->tx_pipe = c2s;
    sock->is_connected = 1;

    pc = unix_create_pending_conn(c2s, s2c, proc->pid, fd);
    if (!pc) {
      serial_write("[syscall] connect: failed to create pending connection\n");
      fs_socket_put_pipes(sock);
      return -1;
    }

    if (unix_enqueue_pending(lst, pc) < 0) {
      serial_write("[syscall] connect: failed to enqueue pending connection\n");
      unix_free_pending(pc);
      fs_socket_put_pipes(sock);
      return -1;
    }

    serial_write("[syscall] connect: unix connection enqueued successfully\n");
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_accept(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  void *addr = (void *)args->arg3;
  uint64_t *addrlen = (uint64_t *)args->arg4;
  process_fd_socket_t *sock;
  int newfd;

  extern void serial_write(const char *str);
  extern void serial_write_num(uint64_t n);

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;
  sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock || !sock->is_listening)
    return -1;


  if (sock->domain == 2) {
    if (sock->accept_queue_count == 0) {
      return (uint64_t)-2;
    }

    process_fd_socket_t *client = (process_fd_socket_t *)sock->accept_queue[0];
    for (int i = 1; i < sock->accept_queue_count; i++) {
      sock->accept_queue[i - 1] = sock->accept_queue[i];
    }
    sock->accept_queue_count--;
    sock->accept_queue[sock->accept_queue_count] = NULL;

    newfd = fs_alloc_fd_slot(proc, 0);
    if (newfd < 0) {
      for (int i = sock->accept_queue_count; i > 0; i--) {
        sock->accept_queue[i] = sock->accept_queue[i - 1];
      }
      sock->accept_queue[0] = client;
      sock->accept_queue_count++;
      return -1;
    }

    proc->fds[newfd] = client;
    proc->fd_kind[newfd] = PROC_FD_KIND_SOCKET;
    proc->fd_flags[newfd] = O_RDWR;

    if (addr && addrlen) {
      if (*addrlen >= 8) {
        uint8_t *a_bytes = (uint8_t *)addr;
        *(uint16_t *)a_bytes = 2; // AF_INET
        if (client->pcb) {
          uint16_t remote_port = 0;
          uint32_t remote_ip = 0;
          extern void network_socket_get_remote_info(void *sock, uint16_t *port,
                                                     uint32_t *ip);
          network_socket_get_remote_info(client, &remote_port, &remote_ip);

          uint16_t sin_port =
              ((remote_port & 0xFF) << 8) | ((remote_port >> 8) & 0xFF);
          *(uint16_t *)(a_bytes + 2) = sin_port;
          *(uint32_t *)(a_bytes + 4) = remote_ip;
        }
      }
    }
    return newfd;
  } else {
    unix_listener_t *lst;
    unix_pending_conn_t *pc;

    lst = unix_find_listener(sock->path);
    if (!lst || !unix_listener_is_listening(lst))
      return -1;

    pc = unix_dequeue_pending(lst);
    if (!pc)
      return -2;

    newfd = fs_alloc_fd_slot(proc, 0);
    if (newfd < 0) {
      serial_write("[syscall] accept: no free fd slot for UNIX client\n");
      unix_enqueue_pending(lst, pc);
      return -1;
    }

    process_fd_socket_t *child = process_socket_create();
    if (!child) {
      unix_enqueue_pending(lst, pc);
      return -1;
    }

    serial_write(
        "[syscall] accept: UNIX connection dequeued, allocating child fd=");
    serial_write_num(newfd);
    serial_write("\n");

    child->rx_pipe = (process_fd_pipe_t *)pc->pipe1;
    child->tx_pipe = (process_fd_pipe_t *)pc->pipe2;
    child->is_connected = 1;
    child->domain = 1;
    proc->fds[newfd] = child;
    proc->fd_kind[newfd] = PROC_FD_KIND_SOCKET;
    proc->fd_flags[newfd] = O_RDWR;

    unix_free_pending(pc);
    return newfd;
  }
}

static uint64_t fs_cmd_unix_socket_close(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;
  if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    process_socket_release((process_fd_socket_t *)proc->fds[fd]);
    proc->fds[fd] = NULL;
    proc->fd_kind[fd] = PROC_FD_KIND_NONE;
    proc->fd_flags[fd] = 0;
    return 0;
  }
  return -1;
}

static uint64_t fs_cmd_unix_socket_unlink(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  if (!path)
    return -1;
  return unix_unregister_listener(path);
}

static uint64_t fs_cmd_open(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  const char *mode = (const char *)args->arg3;
  if (!path || !mode)
    return -1;

  extern void serial_write(const char *str);
  extern void serial_write_hex(uint64_t value);
  extern void serial_write_num(uint64_t num);

  // vfs_open now handles normalization internally with process_get_current()
  // but let's be explicit if we can.
  vfs_file_t *vf = vfs_open(path, mode);

  if (!vf)
    return -1;

  process_fd_file_ref_t *ref =
      (process_fd_file_ref_t *)kmalloc(sizeof(process_fd_file_ref_t));
  if (!ref) {
    vfs_close(vf);
    return -1;
  }
  ref->file = vf;
  ref->refs = 1;

  for (int i = 0; i < MAX_PROCESS_FDS; i++) {
    if (proc->fds[i] == NULL) {
      proc->fds[i] = ref;
      proc->fd_kind[i] = PROC_FD_KIND_FILE;
      proc->fd_flags[i] = fs_mode_to_flags(mode);
      return (uint64_t)i;
    }
  }

  kfree(ref);
  vfs_close(vf);
  return -1;
}

static uint64_t fs_cmd_read(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  void *buf = (void *)args->arg3;
  uint32_t len = (uint32_t)args->arg4;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
    if (!ref || !ref->file)
      return -1;
    return (uint64_t)vfs_read(ref->file, buf, (int)len);
  }

  if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    if (!pipe || !buf)
      return -1;
    uint8_t *out = (uint8_t *)buf;
    uint32_t n = 0;
    while (n < len) {
      if (pipe->count == 0) {
        if (pipe->writers == 0)
          break;
        if (proc->fd_flags[fd] & O_NONBLOCK) {
          if (n == 0)
            return (uint64_t)-2;
          break;
        }
        break;
      }
      out[n++] = pipe->data[pipe->read_pos];
      pipe->read_pos = (pipe->read_pos + 1) % sizeof(pipe->data);
      pipe->count--;
    }
    if (n > 0) {
      wait_queue_wake_all(&pipe->write_queue);
    }
    return n;
  }

  if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
    if (!sock)
      return -1;
    if (sock->domain == 2) {
      int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
      int ret = network_socket_recv(sock, buf, len, nonblock);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    }

    process_fd_pipe_t *pipe = sock->rx_pipe;
    if (!pipe || !buf)
      return -1;
    uint8_t *out = (uint8_t *)buf;
    uint32_t n = 0;
    asm volatile("sti");
    while (n < len) {
      if (pipe->count == 0) {
        if (pipe->writers == 0)
          break;
        if (proc->fd_flags[fd] & O_NONBLOCK) {
          if (n == 0)
            return (uint64_t)-2;
          break;
        }
        if (n > 0)
          break;
        k_delay(10);
        continue;
      }
      out[n++] = pipe->data[pipe->read_pos];
      pipe->read_pos = (pipe->read_pos + 1) % sizeof(pipe->data);
      pipe->count--;
    }
    if (n > 0) {
      wait_queue_wake_all(&pipe->write_queue);
    }
    return n;
  }

  return -1;
}

static uint64_t fs_cmd_write(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  const void *buf = (const void *)args->arg3;
  uint32_t len = (uint32_t)args->arg4;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
    if (!ref || !ref->file)
      return -1;
    return (uint64_t)vfs_write(ref->file, buf, (int)len);
  }

  if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    if (!pipe || !buf)
      return -1;
    if (pipe->readers <= 0)
      return (uint64_t)-1;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t n = 0;
    while (n < len) {
      if (pipe->count == sizeof(pipe->data)) {
        if (proc->fd_flags[fd] & O_NONBLOCK) {
          if (n == 0)
            return (uint64_t)-2;
          break;
        }
        break;
      }
      pipe->data[pipe->write_pos] = in[n++];
      pipe->write_pos = (pipe->write_pos + 1) % sizeof(pipe->data);
      pipe->count++;
    }
    if (n > 0) {
      wait_queue_wake_all(&pipe->read_queue);
    }
    return n;
  }

  if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
    if (!sock)
      return -1;
    if (sock->domain == 2) {
      int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
      int ret = network_socket_send(sock, buf, len, nonblock);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    }

    process_fd_pipe_t *pipe = sock->tx_pipe;
    if (!pipe || !buf)
      return -1;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t n = 0;
    asm volatile("sti");
    while (n < len) {
      if (pipe->count == sizeof(pipe->data)) {
        if (pipe->readers <= 0)
          break;
        if (proc->fd_flags[fd] & O_NONBLOCK) {
          if (n == 0)
            return (uint64_t)-2;
          break;
        }
        if (n > 0)
          break;
        k_delay(10);
        continue;
      }
      pipe->data[pipe->write_pos] = in[n++];
      pipe->write_pos = (pipe->write_pos + 1) % sizeof(pipe->data);
      pipe->count++;
    }
    if (n > 0) {
      wait_queue_wake_all(&pipe->read_queue);
    }
    return n;
  }

  return -1;
}

static uint64_t fs_cmd_close(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
    if (ref) {
      ref->refs--;
      if (ref->refs <= 0) {
        if (ref->file)
          vfs_close(ref->file);
        kfree(ref);
      }
    }
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ ||
             proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    if (pipe) {
      if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ)
        pipe->readers--;
      else
        pipe->writers--;
      if (pipe->readers <= 0 && pipe->writers <= 0) {
        kfree(pipe);
      }
    }
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    process_socket_release((process_fd_socket_t *)proc->fds[fd]);
  }

  proc->fds[fd] = NULL;
  proc->fd_kind[fd] = PROC_FD_KIND_NONE;
  proc->fd_flags[fd] = 0;
  return 0;
}

static uint64_t fs_cmd_seek(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  int offset = (int)args->arg3;
  int whence = (int)args->arg4; // 0=SET, 1=CUR, 2=END
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;
  if (proc->fd_kind[fd] != PROC_FD_KIND_FILE)
    return -1;
  process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
  if (!ref || !ref->file)
    return -1;
  return (uint64_t)vfs_seek(ref->file, offset, whence);
}

static uint64_t fs_cmd_tell(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;
  if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ ||
      proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    return pipe ? pipe->count : 0;
  }
  if (proc->fd_kind[fd] != PROC_FD_KIND_FILE)
    return -1;
  process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
  if (!ref || !ref->file)
    return -1;
  return (uint64_t)vfs_file_position(ref->file);
}

static uint64_t fs_cmd_size(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;
  if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ ||
      proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    return pipe ? pipe->count : 0;
  }
  if (proc->fd_kind[fd] != PROC_FD_KIND_FILE)
    return -1;
  process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
  if (!ref || !ref->file)
    return -1;
  return (uint64_t)vfs_file_size(ref->file);
}

static uint64_t fs_cmd_dup(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int oldfd = (int)args->arg2;
  if (oldfd < 0 || oldfd >= MAX_PROCESS_FDS || !proc->fds[oldfd])
    return -1;

  int newfd = fs_alloc_fd_slot(proc, 0);
  if (newfd < 0)
    return -1;

  proc->fds[newfd] = proc->fds[oldfd];
  proc->fd_kind[newfd] = proc->fd_kind[oldfd];
  proc->fd_flags[newfd] = proc->fd_flags[oldfd];

  if (proc->fd_kind[oldfd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[oldfd];
    if (ref)
      ref->refs++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_PIPE_READ) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[oldfd];
    if (pipe)
      pipe->readers++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[oldfd];
    if (pipe)
      pipe->writers++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_SOCKET) {
    process_socket_addref((process_fd_socket_t *)proc->fds[oldfd]);
  }

  return (uint64_t)newfd;
}

static uint64_t fs_cmd_dup2(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int oldfd = (int)args->arg2;
  int newfd = (int)args->arg3;
  if (oldfd < 0 || oldfd >= MAX_PROCESS_FDS || !proc->fds[oldfd])
    return -1;
  if (newfd < 0 || newfd >= MAX_PROCESS_FDS)
    return -1;
  if (oldfd == newfd)
    return (uint64_t)newfd;

  if (proc->fds[newfd]) {
    syscall_args_t close_args = *args;
    close_args.arg2 = (uint64_t)newfd;
    if (fs_cmd_close(&close_args) != 0)
      return -1;
  }

  proc->fds[newfd] = proc->fds[oldfd];
  proc->fd_kind[newfd] = proc->fd_kind[oldfd];
  proc->fd_flags[newfd] = proc->fd_flags[oldfd];

  if (proc->fd_kind[oldfd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[oldfd];
    if (ref)
      ref->refs++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_PIPE_READ) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[oldfd];
    if (pipe)
      pipe->readers++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[oldfd];
    if (pipe)
      pipe->writers++;
  } else if (proc->fd_kind[oldfd] == PROC_FD_KIND_SOCKET) {
    process_socket_addref((process_fd_socket_t *)proc->fds[oldfd]);
  }

  return (uint64_t)newfd;
}

static uint64_t fs_cmd_pipe(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int *pipefd = (int *)args->arg2;
  if (!pipefd)
    return -1;

  int rfd = fs_alloc_fd_slot(proc, 0);
  if (rfd < 0)
    return -1;
  int wfd = fs_alloc_fd_slot(proc, rfd + 1);
  if (wfd < 0)
    return -1;

  process_fd_pipe_t *pipe = fs_create_pipe_state();
  if (!pipe)
    return -1;

  proc->fds[rfd] = pipe;
  proc->fd_kind[rfd] = PROC_FD_KIND_PIPE_READ;
  proc->fd_flags[rfd] = O_RDONLY;

  proc->fds[wfd] = pipe;
  proc->fd_kind[wfd] = PROC_FD_KIND_PIPE_WRITE;
  proc->fd_flags[wfd] = O_WRONLY;

  pipefd[0] = rfd;
  pipefd[1] = wfd;
  return 0;
}

static uint64_t fs_cmd_fcntl(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  int cmd = (int)args->arg3;
  int val = (int)args->arg4;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  if (cmd == F_GETFL) {
    return (uint64_t)proc->fd_flags[fd];
  }
  if (cmd == F_SETFL) {
    proc->fd_flags[fd] = (proc->fd_flags[fd] & ~(O_APPEND | O_NONBLOCK)) |
                         (val & (O_APPEND | O_NONBLOCK));
    return 0;
  }
  return -1;
}

static uint64_t fs_cmd_list(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  FAT32_FileInfo *u_entries = (FAT32_FileInfo *)args->arg3;
  int max_entries = (int)args->arg4;
  if (!path || !u_entries)
    return -1;

  char normalized[VFS_MAX_PATH];
  vfs_normalize_path(proc->cwd, path, normalized);

  // Safety cap for kernel allocation
  if (max_entries > 256)
    max_entries = 256;
  if (max_entries <= 0)
    return 0;

  vfs_dirent_t *v_entries =
      (vfs_dirent_t *)kmalloc(sizeof(vfs_dirent_t) * max_entries);
  if (!v_entries)
    return -1;

  int count = vfs_list_directory(normalized, v_entries, max_entries, 0);
  if (count > 0) {
    for (int i = 0; i < count; i++) {
      strcpy(u_entries[i].name, v_entries[i].name);
      u_entries[i].size = v_entries[i].size;
      u_entries[i].is_directory = v_entries[i].is_directory;
      u_entries[i].start_cluster = v_entries[i].start_cluster;
      u_entries[i].write_date = v_entries[i].write_date;
      u_entries[i].write_time = v_entries[i].write_time;
    }
  }
  kfree(v_entries);
  return (uint64_t)count;
}

static uint64_t fs_cmd_list_offset(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  FAT32_FileInfo *u_entries = (FAT32_FileInfo *)args->arg3;
  int max_entries = (int)args->arg4;
  int offset = (int)args->arg5;
  if (!path || !u_entries)
    return -1;

  char normalized[VFS_MAX_PATH];
  vfs_normalize_path(proc->cwd, path, normalized);

  if (max_entries > 256)
    max_entries = 256;
  if (max_entries <= 0)
    return 0;

  vfs_dirent_t *v_entries =
      (vfs_dirent_t *)kmalloc(sizeof(vfs_dirent_t) * max_entries);
  if (!v_entries)
    return -1;

  int count = vfs_list_directory(normalized, v_entries, max_entries, offset);
  if (count > 0) {
    for (int i = 0; i < count; i++) {
      // Direct copy as layouts are now aligned
      strcpy(u_entries[i].name, v_entries[i].name);
      u_entries[i].size = v_entries[i].size;
      u_entries[i].is_directory = v_entries[i].is_directory;
      u_entries[i].start_cluster = v_entries[i].start_cluster;
      u_entries[i].write_date = v_entries[i].write_date;
      u_entries[i].write_time = v_entries[i].write_time;
    }
  }
  kfree(v_entries);
  return (uint64_t)count;
}

static uint64_t fs_cmd_delete(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  if (!path)
    return -1;
  char normalized[VFS_MAX_PATH];
  vfs_normalize_path(proc->cwd, path, normalized);
  if (vfs_is_directory(normalized)) {
    return vfs_rmdir(normalized) ? 0 : -1;
  }
  if (vfs_delete(normalized))
    return 0;
  return unix_unregister_listener(normalized);
}

static uint64_t fs_cmd_get_info(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  FAT32_FileInfo *u_info = (FAT32_FileInfo *)args->arg3;
  if (!path || !u_info)
    return -1;

  char normalized[VFS_MAX_PATH];
  vfs_normalize_path(proc->cwd, path, normalized);

  vfs_dirent_t v_info;
  int res = vfs_get_info(normalized, &v_info);
  if (res == 0) {
    strcpy(u_info->name, v_info.name);
    u_info->size = v_info.size;
    u_info->is_directory = v_info.is_directory;
    u_info->start_cluster = v_info.start_cluster;
    u_info->write_date = v_info.write_date;
    u_info->write_time = v_info.write_time;
  }
  return (uint64_t)res;
}

static uint64_t fs_cmd_mkdir(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  if (!path)
    return -1;
  if (vfs_exists(path)) {
    return (uint64_t)-17;
  }
  return vfs_mkdir(path) ? 0 : -1;
}

static uint64_t fs_cmd_exists(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  if (!path)
    return 0;
  return vfs_exists(path) ? 1 : 0;
}

static uint64_t fs_cmd_getcwd(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  char *buf = (char *)args->arg2;
  int size = (int)args->arg3;
  if (!buf || size <= 0)
    return -1;
  int len = (int)strlen(proc->cwd);
  if (len >= size)
    return -1;
  strcpy(buf, proc->cwd);
  return (uint64_t)len;
}

static uint64_t fs_cmd_chdir(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  if (!path)
    return -1;
  char normalized[VFS_MAX_PATH];
  vfs_normalize_path(proc->cwd, path, normalized);
  if (vfs_is_directory(normalized)) {
    strcpy(proc->cwd, normalized);
    return 0;
  }
  return -1;
}
static uint64_t fs_cmd_statfs(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  vfs_statfs_t *stat = (vfs_statfs_t *)args->arg3;
  if (!path || !stat)
    return -1;
  return vfs_statfs(path, stat) == 0 ? 0 : -1;
}

static uint64_t fs_cmd_mount_count(const syscall_args_t *args) {
  (void)args;
  return (uint64_t)vfs_get_mount_count();
}

typedef struct {
  char path[256];
  char device[32];
  char fs_type[16];
} syscall_mount_info_t;

static uint64_t fs_cmd_mount_info(const syscall_args_t *args) {
  int index = (int)args->arg2;
  syscall_mount_info_t *info = (syscall_mount_info_t *)args->arg3;
  if (!info)
    return -1;

  vfs_mount_t *m = vfs_get_mount(index);
  if (!m)
    return -1;

  strcpy(info->path, m->path);
  strcpy(info->device, m->device);
  strcpy(info->fs_type, m->fs_type);
  return 0;
}

void poll_cleanup(process_t *proc) {
  if (!proc)
    return;
  poll_wtable_t *wt = &proc->poll_table;
  for (int i = 0; i < wt->count; i++) {
    wait_queue_remove(wt->entries[i].h, &wt->entries[i].entry);
  }
  wt->count = 0;
}

static void poll_qproc(wait_queue_head_t *h, poll_table_t *pt) {
  (void)pt;
  process_t *proc = process_get_current();
  poll_wtable_t *wt = &proc->poll_table;
  if (wt->count < MAX_POLL_ENTRIES) {
    poll_entry_t *pe = &wt->entries[wt->count++];
    pe->h = h;
    pe->entry.proc = proc;
    pe->entry.next = NULL;
    wait_queue_add(h, &pe->entry);
  }
}

static uint64_t fs_cmd_poll(const syscall_args_t *args) {
  struct pollfd *fds = (struct pollfd *)args->arg2;
  int nfds = (int)args->arg3;
  int timeout = (int)args->arg4;

  process_t *proc = process_get_current();

  if (!proc || !fds || nfds <= 0 || nfds > 128) {
    return -1;
  }

  // Initialize/reset poll table in process structure
  proc->poll_table.pt.qproc = poll_qproc;
  proc->poll_table.count = 0;
  poll_table_t *pt = &proc->poll_table.pt;

  int ready = 0;
  for (int i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    fds[i].revents = 0;
    if (fd < 0 || fd >= MAX_PROCESS_FDS)
      continue;
    if (!proc->fds[fd]) {
      fds[i].revents = POLLNVAL;
      ready++;
      continue;
    }

    int mask = 0;
    if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
      process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
      mask = vfs_poll(ref->file, pt);
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ ||
               proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
      process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
      if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ) {
        if (pt->qproc)
          pt->qproc(&pipe->read_queue, pt);
        if (pipe->count > 0)
          mask |= POLLIN;
        if (pipe->writers == 0)
          mask |= POLLHUP;
      } else {
        if (pt->qproc)
          pt->qproc(&pipe->write_queue, pt);
        if (pipe->count < sizeof(pipe->data))
          mask |= POLLOUT;
        if (pipe->readers == 0)
          mask |= POLLERR;
      }
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
      process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
      if (sock) {
        if (sock->domain == 2) {
          if (sock->is_listening) {
            if (pt->qproc)
              pt->qproc(&sock->accept_waitq, pt);
            if (sock->accept_queue_count > 0)
              mask |= POLLIN;
          } else {
            if (pt->qproc)
              pt->qproc(&sock->rx_waitq, pt);
            if (sock->recv_queue != NULL)
              mask |= POLLIN;
            if (sock->tcp_closed)
              mask |= (POLLIN | POLLHUP);
            if (sock->tcp_connect_error)
              mask |= POLLERR;
            if (sock->is_connected)
              mask |= POLLOUT;
          }
        } else {
          if (sock->is_listening) {
            unix_listener_t *lst = unix_find_listener(sock->path);
            if (lst) {
              wait_queue_head_t *wq = unix_listener_get_accept_waitq(lst);
              if (pt->qproc && wq)
                pt->qproc(wq, pt);
              if (unix_listener_has_pending(lst))
                mask |= POLLIN;
            }
          } else if (sock->rx_pipe && sock->tx_pipe) {
            if (pt->qproc)
              pt->qproc(&sock->rx_pipe->read_queue, pt);
            if (pt->qproc)
              pt->qproc(&sock->tx_pipe->write_queue, pt);
            if (sock->rx_pipe->count > 0)
              mask |= POLLIN;
            if (sock->tx_pipe->count < sizeof(sock->tx_pipe->data))
              mask |= POLLOUT;
            if (sock->rx_pipe->writers == 0)
              mask |= POLLHUP;
            if (sock->tx_pipe->readers == 0)
              mask |= POLLERR;
          }
        }
      }
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_TTY) {
      extern int tty_poll(int tty_id, struct poll_table *pt);
      mask = tty_poll(proc->tty_id, pt);
    }

    fds[i].revents = mask & fds[i].events;
    if (fds[i].revents)
      ready++;
  }

  if (ready > 0 || timeout == 0) {
    poll_cleanup(proc);
    return (uint64_t)ready;
  }

  if (timeout > 0) {
    extern uint32_t get_ticks(void);
    uint32_t ticks = timeout / 16;
    if (ticks == 0)
      ticks = 1;
    proc->sleep_until = get_ticks() + ticks;
  }

  proc->state = PROC_STATE_BLOCKED;
  return (uint64_t)-2;
}



static uint64_t fs_cmd_ioctl(const syscall_args_t *args) {
  int fd = (int)args->arg2;
  uint64_t request = args->arg3;
  void *arg = (void *)args->arg4;

  process_t *proc = process_get_current();
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
    extern int vfs_ioctl(vfs_file_t * file, uint64_t request, void *arg);
    return (uint64_t)vfs_ioctl(ref->file, request, arg);
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_TTY) {
    extern int tty_ioctl(int id, uint64_t request, void *arg);
    return (uint64_t)tty_ioctl(proc->tty_id, request, arg);
  }

  return -1;
}

#define FS_CMD_TABLE_SIZE 35
static const syscall_handler_fn fs_cmd_table[FS_CMD_TABLE_SIZE] = {
    [FS_CMD_OPEN] = fs_cmd_open,               // 1
    [FS_CMD_READ] = fs_cmd_read,               // 2
    [FS_CMD_WRITE] = fs_cmd_write,             // 3
    [FS_CMD_CLOSE] = fs_cmd_close,             // 4
    [FS_CMD_SEEK] = fs_cmd_seek,               // 5
    [FS_CMD_TELL] = fs_cmd_tell,               // 6
    [FS_CMD_LIST] = fs_cmd_list,               // 7
    [FS_CMD_DELETE] = fs_cmd_delete,           // 8
    [FS_CMD_SIZE] = fs_cmd_size,               // 9
    [FS_CMD_MKDIR] = fs_cmd_mkdir,             // 10
    [FS_CMD_EXISTS] = fs_cmd_exists,           // 11
    [FS_CMD_GETCWD] = fs_cmd_getcwd,           // 12
    [FS_CMD_CHDIR] = fs_cmd_chdir,             // 13
    [FS_CMD_GET_INFO] = fs_cmd_get_info,       // 14
    [FS_CMD_DUP] = fs_cmd_dup,                 // 15
    [FS_CMD_DUP2] = fs_cmd_dup2,               // 16
    [FS_CMD_PIPE] = fs_cmd_pipe,               // 17
    [FS_CMD_FCNTL] = fs_cmd_fcntl,             // 18
    [FS_CMD_STATFS] = fs_cmd_statfs,           // 19
    [FS_CMD_MOUNT_COUNT] = fs_cmd_mount_count, // 20
    [FS_CMD_MOUNT_INFO] = fs_cmd_mount_info,   // 21
    [FS_CMD_POLL] = fs_cmd_poll,               // 22
    [FS_CMD_IOCTL] = fs_cmd_ioctl,             // 24
    [FS_CMD_UNIX_SOCKET_CREATE] = fs_cmd_unix_socket_create,
    [FS_CMD_UNIX_SOCKET_BIND] = fs_cmd_unix_socket_bind,
    [FS_CMD_UNIX_SOCKET_LISTEN] = fs_cmd_unix_socket_listen,
    [FS_CMD_UNIX_SOCKET_ACCEPT] = fs_cmd_unix_socket_accept,
    [FS_CMD_UNIX_SOCKET_CONNECT] = fs_cmd_unix_socket_connect,
    [FS_CMD_UNIX_SOCKET_CLOSE] = fs_cmd_unix_socket_close,
    [FS_CMD_UNIX_SOCKET_UNLINK] = fs_cmd_unix_socket_unlink,
    [FS_CMD_LIST_OFFSET] = fs_cmd_list_offset, // 34
};

static uint64_t sys_cmd_rtc_get(const syscall_args_t *args) {
  int *dt = (int *)args->arg2;
  if (!dt)
    return -1;
  extern void rtc_get_datetime(int *y, int *m, int *d, int *h, int *min,
                               int *s);
  rtc_get_datetime(&dt[0], &dt[1], &dt[2], &dt[3], &dt[4], &dt[5]);
  return 0;
}

static uint64_t sys_cmd_reboot(const syscall_args_t *args) {
  (void)args;
  k_reboot();
  return 0;
}

static uint64_t sys_cmd_shutdown(const syscall_args_t *args) {
  (void)args;
  k_shutdown();
  return 0;
}
 

static uint64_t sys_cmd_get_mem_info(const syscall_args_t *args) {
  uint64_t *out = (uint64_t *)args->arg2;
  if (!out)
    return -1;
  MemStats stats = memory_get_stats();
  out[0] = (uint64_t)stats.total_memory;
  out[1] = (uint64_t)stats.used_memory;
  return 0;
}

static uint64_t sys_cmd_get_ticks(const syscall_args_t *args) {
  (void)args;
  extern uint32_t get_ticks(void);
  return (uint64_t)get_ticks();
}

static uint64_t sys_cmd_pci_list(const syscall_args_t *args) {
  typedef struct {
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
  } pci_info_t;
  pci_info_t *info = (pci_info_t *)args->arg2;
  int idx = (int)args->arg3;
  if (!info) {
    pci_device_t pci_devs[128];
    return pci_enumerate_devices(pci_devs, 128);
  }
  pci_device_t pci_devs[128];
  int count = pci_enumerate_devices(pci_devs, 128);
  if (idx >= 0 && idx < count) {
    info->vendor = pci_devs[idx].vendor_id;
    info->device = pci_devs[idx].device_id;
    info->class_code = pci_devs[idx].class_code;
    info->subclass = pci_devs[idx].subclass;
    return 0;
  }
  return -1;
}
static uint64_t sys_cmd_udp_send(const syscall_args_t *args) {
  ipv4_address_t *dest_ip = (ipv4_address_t *)args->arg2;
  uint32_t ports = (uint32_t)args->arg3;
  uint16_t dest_port = ports & 0xFFFF;
  uint16_t src_port = (ports >> 16) & 0xFFFF;
  const void *data = (const void *)args->arg4;
  size_t data_len = (size_t)args->arg5;
  if (!dest_ip || !data)
    return -1;
  return udp_send_packet(dest_ip, dest_port, src_port, data, data_len);
}
static uint64_t sys_cmd_icmp_ping(const syscall_args_t *args) {
  ipv4_address_t *dest_ip = (ipv4_address_t *)args->arg2;
  if (!dest_ip)
    return -1;
  extern int network_icmp_single_ping(ipv4_address_t * dest);
  return (uint64_t)network_icmp_single_ping(dest_ip);
}
static uint64_t sys_cmd_set_text_color(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  uint32_t color = (uint32_t)args->arg2;
  if (proc->is_terminal_proc && proc->tty_id >= 0) {
    char seq[32];
    int pos = 0;
    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;

    seq[pos++] = 0x1B;
    seq[pos++] = '[';
    seq[pos++] = '3';
    seq[pos++] = '8';
    seq[pos++] = ';';
    seq[pos++] = '2';
    seq[pos++] = ';';

    char num[8];
    itoa(r, num);
    for (int i = 0; num[i] && pos < (int)sizeof(seq) - 1; i++)
      seq[pos++] = num[i];
    seq[pos++] = ';';
    itoa(g, num);
    for (int i = 0; num[i] && pos < (int)sizeof(seq) - 1; i++)
      seq[pos++] = num[i];
    seq[pos++] = ';';
    itoa(b, num);
    for (int i = 0; num[i] && pos < (int)sizeof(seq) - 1; i++)
      seq[pos++] = num[i];
    seq[pos++] = 'm';

    tty_write(proc->tty_id, seq, (size_t)pos);
    return 0;
  }
  cmd_set_current_color(color);
  return 0;
}

static uint64_t sys_cmd_rtc_set(const syscall_args_t *args) {
  int *dt = (int *)args->arg2;
  if (!dt)
    return -1;
  extern void rtc_set_datetime(int y, int m, int d, int h, int min, int s);
  rtc_set_datetime(dt[0], dt[1], dt[2], dt[3], dt[4], dt[5]);
  return 0;
}

static uint64_t sys_cmd_tcp_connect(const syscall_args_t *args) {
  ipv4_address_t *ip = (ipv4_address_t *)args->arg2;
  uint16_t port = (uint16_t)args->arg3;
  extern int network_tcp_connect(const ipv4_address_t *ip, uint16_t port);
  return (uint64_t)network_tcp_connect(ip, port);
}

static uint64_t sys_cmd_tcp_send(const syscall_args_t *args) {
  const void *data = (const void *)args->arg2;
  size_t len = (size_t)args->arg3;
  extern int network_tcp_send(const void *data, size_t len);
  return (uint64_t)network_tcp_send(data, len);
}

static uint64_t sys_cmd_tcp_recv(const syscall_args_t *args) {
  void *buf = (void *)args->arg2;
  size_t max_len = (size_t)args->arg3;
  extern int network_tcp_recv(void *buf, size_t max_len);
  return (uint64_t)network_tcp_recv(buf, max_len);
}

static uint64_t sys_cmd_tcp_close(const syscall_args_t *args) {
  (void)args;
  extern int network_tcp_close(void);
  return (uint64_t)network_tcp_close();
}

static uint64_t sys_cmd_dns_lookup(const syscall_args_t *args) {
  const char *user_name = (const char *)args->arg2;
  ipv4_address_t *out_ip = (ipv4_address_t *)args->arg3;
  char name_buf[256];
  int i = 0;
  while (i < 255 && user_name[i]) {
    name_buf[i] = user_name[i];
    i++;
  }
  name_buf[i] = 0;
  extern int network_dns_lookup(const char *name, ipv4_address_t *out_ip);
  return (uint64_t)network_dns_lookup(name_buf, out_ip);
}

static uint64_t sys_cmd_net_unlock(const syscall_args_t *args) {
  (void)args;
  extern void network_force_unlock(void);
  network_force_unlock();
  return 0;
}


static uint64_t sys_cmd_set_raw_mode(const syscall_args_t *args) {
  extern void cmd_set_raw_mode(bool enabled);
  cmd_set_raw_mode((bool)args->arg2);
  return 0;
}

static uint64_t sys_cmd_tcp_recv_nb(const syscall_args_t *args) {
  void *buf = (void *)args->arg2;
  size_t max_len = (size_t)args->arg3;
  extern int network_tcp_recv_nb(void *buf, size_t max_len);
  return (uint64_t)network_tcp_recv_nb(buf, max_len);
}

static uint64_t sys_cmd_tcp_listen(const syscall_args_t *args) {
  uint16_t port = (uint16_t)args->arg2;
  extern int network_tcp_listen(uint16_t port);
  return (uint64_t)network_tcp_listen(port);
}

static uint64_t sys_cmd_tcp_accept(const syscall_args_t *args) {
  (void)args;
  extern int network_tcp_accept(void);
  return (uint64_t)network_tcp_accept();
}

static uint64_t sys_cmd_parallel_run(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  void (*user_fn)(void *) = (void (*)(void *))args->arg2;
  void **user_args = (void **)args->arg3;
  int count = (int)args->arg4;

  if (count <= 0)
    return 0;
  if (count > 64)
    count = 64;

  volatile int completion_counter = count;
  uint64_t current_pml4 = proc->pml4_phys;

  smp_user_task_t tasks[64];

  for (int i = 0; i < count; i++) {
    tasks[i].fn = user_fn;
    tasks[i].arg = user_args[i];
    tasks[i].pml4_phys = current_pml4;
    tasks[i].completion_counter = &completion_counter;

    extern void work_queue_submit(void (*fn)(void *), void *arg);
    work_queue_submit(smp_user_wrapper, &tasks[i]);
  }

  extern bool work_queue_drain_one(void);
  while (completion_counter > 0) {
    if (!work_queue_drain_one()) {
      asm volatile("pause");
    }
  }
  return 0;
}

static uint64_t sys_cmd_tty_create(const syscall_args_t *args) {
  (void)args;
  return tty_create();
}

static uint64_t sys_cmd_tty_get_id(const syscall_args_t *args) {
  (void)args;
  process_t *proc = process_get_current();
  if (!proc)
    return (uint64_t)-1;
  return (uint64_t)proc->tty_id;
}

static uint64_t sys_cmd_set_fs_base(const syscall_args_t *args) {
  uint64_t fs_base = args->arg2;
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user)
    return (uint64_t)-1;
  proc->fs_base = fs_base;
  wrmsr(MSR_FS_BASE, fs_base);
  return 0;
}

static uint64_t sys_cmd_tty_read_out(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  char *buf = (char *)args->arg3;
  size_t len = (size_t)args->arg4;
  if (!buf || len == 0)
    return 0;
  return tty_read_output(tty_id, buf, len);
}

static uint64_t sys_cmd_tty_write_in(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  const char *buf = (const char *)args->arg3;
  size_t len = (size_t)args->arg4;
  if (!buf || len == 0)
    return 0;
  return tty_write_input(tty_id, buf, len);
}

static uint64_t sys_cmd_tty_read_in(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  char *buf = (char *)args->arg2;
  size_t len = (size_t)args->arg3;
  if (!buf || len == 0)
    return 0;
  if (proc->tty_id < 0)
    return 0;
  return tty_read_input(proc->tty_id, buf, len);
}

static uint64_t sys_cmd_spawn_process(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *user_path = (const char *)args->arg2;
  const char *user_args = (const char *)args->arg3;
  uint64_t flags = args->arg4;
  int tty_id = (int)args->arg5;

  if (!user_path)
    return -1;

  char path_buf[256];
  int pi = 0;
  while (pi < 255 && user_path[pi]) {
    path_buf[pi] = user_path[pi];
    pi++;
  }
  path_buf[pi] = 0;

  char args_buf[512];
  const char *args_ptr = NULL;
  if (user_args) {
    int ai = 0;
    while (ai < 511 && user_args[ai]) {
      args_buf[ai] = user_args[ai];
      ai++;
    }
    args_buf[ai] = 0;
    args_ptr = args_buf;
  }

  bool terminal_proc = (flags & SPAWN_FLAG_TERMINAL) != 0;
  int effective_tty = -1;
  if (flags & SPAWN_FLAG_TTY_ID)
    effective_tty = tty_id;
  else if (flags & SPAWN_FLAG_INHERIT_TTY)
    effective_tty = proc ? proc->tty_id : -1;

  process_t *child =
      process_create_elf(path_buf, args_ptr, terminal_proc, effective_tty);
  if (!child)
    return -1;
  return (uint64_t)child->pid;
}

typedef struct {
  uint64_t sa_handler;
  uint64_t sa_mask;
  int sa_flags;
} k_sigaction_t;

#define SA_RESETHAND 0x80000000
#define SIGKILL_NUM 9

static uint64_t sys_cmd_exec_process(const syscall_args_t *args) {
  const char *user_path = (const char *)args->arg2;
  const char *user_args = (const char *)args->arg3;
  if (!user_path)
    return -1;

  char path_buf[256];
  int pi = 0;
  while (pi < 255 && user_path[pi]) {
    path_buf[pi] = user_path[pi];
    pi++;
  }
  path_buf[pi] = 0;

  char args_buf[512];
  const char *args_ptr = NULL;
  if (user_args) {
    int ai = 0;
    while (ai < 511 && user_args[ai]) {
      args_buf[ai] = user_args[ai];
      ai++;
    }
    args_buf[ai] = 0;
    args_ptr = args_buf;
  }

  return process_exec_replace_current(args->regs, path_buf, args_ptr);
}

static uint64_t sys_cmd_fork_process(const syscall_args_t *args) {
  extern process_t *process_duplicate(registers_t * parent_regs);
  process_t *child = process_duplicate(args->regs);
  if (!child)
    return -1;
  return child->pid;
}

static uint64_t sys_cmd_waitpid(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int pid = (int)args->arg2;
  int *status = (int *)args->arg3;
  int options = (int)args->arg4;
  if (!proc)
    return -1;

  int st = 0;
  int res = process_waitpid(proc->pid, pid, options, &st);
  if (res == -2) {
    if (options & 1)
      return 0; // WNOHANG
    return (uint64_t)-2;
  }
  if (res < 0)
    return (uint64_t)-1;
  if (status)
    *status = st;
  return (uint64_t)res;
}

static uint64_t sys_cmd_kill_signal(const syscall_args_t *args) {
  int pid = (int)args->arg2;
  int sig = (int)args->arg3;
  process_t *target;
  if (pid == -1) {
    target = process_get_current();
  } else {
    target = process_get_by_pid((uint32_t)pid);
  }
  if (!target)
    return -1;
  if (sig == 0)
    return 0;
  if (sig <= 0 || sig >= MAX_SIGNALS)
    return -1;

  if (sig == 9) {
    process_terminate_with_status(target, 128 + sig);
    return 0;
  }

  target->signal_pending |= (1ULL << (uint32_t)sig);
  return 0;
}

static uint64_t sys_cmd_sigaction(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int sig = (int)args->arg2;
  const k_sigaction_t *act = (const k_sigaction_t *)args->arg3;
  k_sigaction_t *oldact = (k_sigaction_t *)args->arg4;
  if (!proc || sig <= 0 || sig >= MAX_SIGNALS)
    return -1;

  if (oldact) {
    oldact->sa_handler = proc->signal_handlers[sig];
    oldact->sa_mask = proc->signal_action_mask[sig];
    oldact->sa_flags = proc->signal_action_flags[sig];
  }
  if (act) {
    if (sig == SIGKILL_NUM && act->sa_handler != 0) {
      return -1;
    }
    proc->signal_handlers[sig] = act->sa_handler;
    proc->signal_action_mask[sig] = act->sa_mask;
    proc->signal_action_flags[sig] = act->sa_flags;
  }
  return 0;
}

static uint64_t sys_cmd_sigprocmask(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int how = (int)args->arg2;
  const uint64_t *set = (const uint64_t *)args->arg3;
  uint64_t *oldset = (uint64_t *)args->arg4;
  if (!proc)
    return -1;

  if (oldset) {
    *oldset = proc->signal_mask;
  }
  if (!set)
    return 0;

  if (how == 0) {
    proc->signal_mask |= *set;
  } else if (how == 1) {
    proc->signal_mask &= ~(*set);
  } else if (how == 2) {
    proc->signal_mask = *set;
  } else {
    return -1;
  }
  proc->signal_mask &= ~(1ULL << SIGKILL_NUM);

  return 0;
}

static uint64_t sys_cmd_sigpending(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  uint64_t *set = (uint64_t *)args->arg2;
  if (!proc || !set)
    return -1;
  *set = proc->signal_pending;
  return 0;
}

static uint64_t sys_cmd_tty_set_fg(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  int pid = (int)args->arg3;
  return tty_set_foreground(tty_id, pid);
}

static uint64_t sys_cmd_tty_get_fg(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  return tty_get_foreground(tty_id);
}

static uint64_t sys_cmd_tty_kill_fg(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  int pid = tty_get_foreground(tty_id);
  if (pid <= 0)
    return 0;
  process_t *target = process_get_by_pid((uint32_t)pid);
  if (target)
    process_terminate(target);
  tty_set_foreground(tty_id, 0);
  return 0;
}

static uint64_t sys_cmd_tty_kill_all(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  process_kill_by_tty(tty_id);
  tty_set_foreground(tty_id, 0);
  return 0;
}

static uint64_t sys_cmd_tty_destroy(const syscall_args_t *args) {
  int tty_id = (int)args->arg2;
  return tty_destroy(tty_id);
}

static uint64_t sys_cmd_pty_create(const syscall_args_t *args) {
  (void)args;
  return (uint64_t)pty_create();
}

static uint64_t sys_cmd_pty_destroy(const syscall_args_t *args) {
  int pty_id = (int)args->arg2;
  return (uint64_t)pty_destroy(pty_id);
}

static uint64_t sys_cmd_get_elf_metadata(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  boredos_app_metadata_t *out = (boredos_app_metadata_t *)args->arg3;
  if (!path || !out)
    return 0;
  return app_metadata_read(path, out) ? 1 : 0;
}
static uint64_t sys_cmd_get_elf_primary_image(const syscall_args_t *args) {
  const char *path = (const char *)args->arg2;
  char *out_path = (char *)args->arg3;
  size_t out_size = (size_t)args->arg4;
  if (!path || !out_path || !out_size)
    return 0;
  return app_metadata_get_primary_image(path, out_path, out_size) ? 1 : 0;
}

static uint64_t sys_cmd_set_keyboard_layout(const syscall_args_t *args) {
  keymap_set_current((keymap_id_t)args->arg2);
  return 0;
}

static uint64_t sys_cmd_get_keyboard_layout(const syscall_args_t *args) {
  (void)args;
  return (uint64_t)keymap_get_current();
}

typedef struct {
  char devname[16];
  char label[32];
  uint32_t type;
  uint32_t total_sectors;
  bool is_partition;
  bool is_fat32;
  bool is_esp;
  uint32_t lba_offset;
} k_disk_info_t;

typedef struct {
  uint32_t lba_start;
  uint32_t sector_count;
  uint8_t part_type;
  uint8_t flags;
  char label[36];
} k_partition_spec_t;

static void disk_k_strcpy(char *dst, const char *src, int max) {
  int i = 0;
  while (i < max - 1 && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

static int disk_k_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}

static uint64_t sys_cmd_disk_get_count(const syscall_args_t *args) {
  (void)args;
  return (uint64_t)disk_get_count();
}

static uint64_t sys_cmd_disk_get_info(const syscall_args_t *args) {
  int index = (int)args->arg2;
  k_disk_info_t *out = (k_disk_info_t *)args->arg3;
  if (!out)
    return (uint64_t)-1;
  Disk *d = disk_get_by_index(index);
  if (!d)
    return (uint64_t)-1;
  disk_k_strcpy(out->devname, d->devname, 16);
  disk_k_strcpy(out->label, d->label, 32);
  out->type = (uint32_t)d->type;
  out->total_sectors = d->total_sectors;
  out->is_partition = d->is_partition;
  out->is_fat32 = d->is_fat32;
  out->is_esp = d->is_esp;
  out->lba_offset = d->partition_lba_offset;
  return 0;
}

static uint64_t sys_cmd_disk_write_gpt(const syscall_args_t *args) {
  const char *devname = (const char *)args->arg2;
  k_partition_spec_t *parts = (k_partition_spec_t *)args->arg3;
  int count = (int)args->arg4;
  if (!devname || !parts)
    return (uint64_t)-1;
  Disk *d = disk_get_by_name(devname);
  if (!d)
    return (uint64_t)-1;
  return (uint64_t)disk_write_gpt(d, (disk_partition_spec_t *)parts, count);
}

static uint64_t sys_cmd_disk_write_mbr(const syscall_args_t *args) {
  const char *devname = (const char *)args->arg2;
  k_partition_spec_t *parts = (k_partition_spec_t *)args->arg3;
  int count = (int)args->arg4;
  if (!devname || !parts)
    return (uint64_t)-1;
  Disk *d = disk_get_by_name(devname);
  if (!d)
    return (uint64_t)-1;
  return (uint64_t)disk_write_mbr(d, (disk_partition_spec_t *)parts, count);
}

static uint64_t sys_cmd_disk_mkfs_fat32(const syscall_args_t *args) {
  extern int mkfs_fat32_format(Disk * disk, uint32_t sector_count,
                               const char *label);
  const char *devname = (const char *)args->arg2;
  const char *label = (const char *)args->arg3;
  if (!devname)
    return (uint64_t)-1;
  Disk *d = disk_get_by_name(devname);
  if (!d)
    return (uint64_t)-1;
  int ret = mkfs_fat32_format(d, d->total_sectors, label);
  if (ret == 0)
    d->is_fat32 = true;
  return (uint64_t)ret;
}

static uint64_t sys_cmd_disk_mount(const syscall_args_t *args) {
  const char *devname = (const char *)args->arg2;
  const char *mountpoint = (const char *)args->arg3;
  if (!devname || !mountpoint)
    return (uint64_t)-1;
  Disk *d = disk_get_by_name(devname);
  if (!d || !d->is_fat32)
    return (uint64_t)-1;
  void *vol = fat32_mount_volume(d);
  if (!vol)
    return (uint64_t)-1;
  if (!vfs_mount(mountpoint, devname, "fat32", fat32_get_realfs_ops(), vol))
    return (uint64_t)-1;
  return 0;
}

static uint64_t sys_cmd_disk_umount(const syscall_args_t *args) {
  const char *mountpoint = (const char *)args->arg2;
  if (!mountpoint)
    return (uint64_t)-1;
  return vfs_umount(mountpoint) ? 0 : (uint64_t)-1;
}

static uint64_t sys_cmd_disk_rescan(const syscall_args_t *args) {
  const char *devname = (const char *)args->arg2;
  if (!devname)
    return (uint64_t)-1;
  Disk *d = disk_get_by_name(devname);
  if (!d)
    return (uint64_t)-1;
  return (uint64_t)disk_rescan(d);
}

static uint64_t sys_cmd_disk_sync(const syscall_args_t *args) {
  const char *mountpoint = (const char *)args->arg2;
  if (!mountpoint)
    return (uint64_t)-1;
  int mc = vfs_get_mount_count();
  for (int i = 0; i < mc; i++) {
    vfs_mount_t *m = vfs_get_mount(i);
    if (m && m->active && disk_k_strcmp(m->path, mountpoint) == 0) {
      Disk *d = disk_get_by_name(m->device);
      if (d)
        return (uint64_t)disk_sync(d);
    }
  }
  return (uint64_t)-1;
}

static uint64_t sys_cmd_disk_replace_kernel(const syscall_args_t *args) {
  extern void serial_write(const char *str);
  const char *src_path = (const char *)args->arg2;
  const char *esp_mountpoint = (const char *)args->arg3;
  if (!src_path || !esp_mountpoint)
    return (uint64_t)-1;

  char dest_path[256];
  int mi = 0;
  while (mi < 255 && esp_mountpoint[mi]) {
    dest_path[mi] = esp_mountpoint[mi];
    mi++;
  }
  const char *suffix = "/boredos.elf";
  for (int i = 0; suffix[i] && mi < 255; i++)
    dest_path[mi++] = suffix[i];
  dest_path[mi] = 0;

  if (disk_k_strcmp(src_path, dest_path) == 0) {
    serial_write("[KUP] Error: source and destination are the same file\n");
    return (uint64_t)-1;
  }

  vfs_file_t *src = vfs_open(src_path, "r");
  if (!src) {
    serial_write("[KUP] Error: source not found\n");
    return (uint64_t)-1;
  }

  uint32_t src_size = vfs_file_size(src);
  if (src_size > 100 * 1024 * 1024) {
    serial_write("[KUP] Error: source > 100 MB\n");
    vfs_close(src);
    return (uint64_t)-1;
  }

  uint8_t magic[4];
  vfs_read(src, magic, 4);
  if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' ||
      magic[3] != 'F') {
    serial_write("[KUP] Error: not an ELF file\n");
    vfs_close(src);
    return (uint64_t)-1;
  }
  vfs_seek(src, 0, 0);

  char bak_path[256];
  mi = 0;
  while (mi < 255 && esp_mountpoint[mi]) {
    bak_path[mi] = esp_mountpoint[mi];
    mi++;
  }
  const char *bak_suffix = "/boredos.elf.bak";
  for (int i = 0; bak_suffix[i] && mi < 255; i++)
    bak_path[mi++] = bak_suffix[i];
  bak_path[mi] = 0;

  vfs_file_t *existing = vfs_open(dest_path, "r");
  if (existing) {
    vfs_file_t *bakf = vfs_open(bak_path, "w");
    if (bakf) {
      uint8_t *cbuf = (uint8_t *)kmalloc(4096);
      if (cbuf) {
        int n;
        while ((n = vfs_read(existing, cbuf, 4096)) > 0)
          vfs_write(bakf, cbuf, n);
        kfree(cbuf);
      }
      vfs_close(bakf);
    } else {
      serial_write("[KUP] Warning: could not create backup\n");
    }
    vfs_close(existing);
  }

  vfs_file_t *dst = vfs_open(dest_path, "w");
  if (!dst) {
    serial_write("[KUP] Error: could not open destination for write\n");
    vfs_close(src);
    return (uint64_t)-1;
  }

  uint8_t *buf = (uint8_t *)kmalloc(4096);
  if (!buf) {
    vfs_close(src);
    vfs_close(dst);
    return (uint64_t)-1;
  }

  uint32_t bytes_written = 0;
  int n;
  while ((n = vfs_read(src, buf, 4096)) > 0) {
    int written = vfs_write(dst, buf, n);
    if (written != n) {
      serial_write("[KUP] Error: write failed mid-copy\n");
      kfree(buf);
      vfs_close(src);
      vfs_close(dst);
      return (uint64_t)-1;
    }
    bytes_written += (uint32_t)written;
  }

  kfree(buf);
  vfs_close(src);
  vfs_close(dst);

  if (bytes_written != src_size) {
    serial_write("[KUP] Error: incomplete write (size mismatch)\n");
    return (uint64_t)-1;
  }

  serial_write("[KUP] Kernel replaced (");
  {
    char numstr[16];
    int ni = 15;
    numstr[ni] = 0;
    uint32_t v = bytes_written;
    if (v == 0) {
      numstr[--ni] = '0';
    } else {
      while (v > 0) {
        numstr[--ni] = '0' + (v % 10);
        v /= 10;
      }
    }
    serial_write(numstr + ni);
  }
  serial_write(" bytes). Backup at boredos.elf.bak. Reboot required.\n");
  return 0;
}

#define SYS_CMD_TABLE_SIZE 110
static const syscall_handler_fn sys_cmd_table[SYS_CMD_TABLE_SIZE] = {
    [SYSTEM_CMD_RTC_GET] = sys_cmd_rtc_get,
    [SYSTEM_CMD_REBOOT] = sys_cmd_reboot,
    [SYSTEM_CMD_SHUTDOWN] = sys_cmd_shutdown,
    [SYSTEM_CMD_GET_MEM_INFO] = sys_cmd_get_mem_info,
    [SYSTEM_CMD_GET_TICKS] = sys_cmd_get_ticks,
    [SYSTEM_CMD_PCI_LIST] = sys_cmd_pci_list,
    [SYSTEM_CMD_UDP_SEND] = sys_cmd_udp_send,
    [SYSTEM_CMD_ICMP_PING] = sys_cmd_icmp_ping,
    [SYSTEM_CMD_SET_TEXT_COLOR] = sys_cmd_set_text_color,
    [SYSTEM_CMD_RTC_SET] = sys_cmd_rtc_set,
    [SYSTEM_CMD_TCP_CONNECT] = sys_cmd_tcp_connect,
    [SYSTEM_CMD_TCP_SEND] = sys_cmd_tcp_send,
    [SYSTEM_CMD_TCP_RECV] = sys_cmd_tcp_recv,
    [SYSTEM_CMD_TCP_CLOSE] = sys_cmd_tcp_close,
    [SYSTEM_CMD_DNS_LOOKUP] = sys_cmd_dns_lookup,
    [SYSTEM_CMD_NET_UNLOCK] = sys_cmd_net_unlock,
    [SYSTEM_CMD_SET_RAW_MODE] = sys_cmd_set_raw_mode,
    [SYSTEM_CMD_TCP_RECV_NB] = sys_cmd_tcp_recv_nb,
    [SYSTEM_CMD_TCP_LISTEN] = sys_cmd_tcp_listen,
    [SYSTEM_CMD_TCP_ACCEPT] = sys_cmd_tcp_accept,
    [SYSTEM_CMD_PARALLEL_RUN] = sys_cmd_parallel_run,
    [SYSTEM_CMD_SET_KEYBOARD_LAYOUT] = sys_cmd_set_keyboard_layout,
    [SYSTEM_CMD_GET_KEYBOARD_LAYOUT] = sys_cmd_get_keyboard_layout,
    [SYSTEM_CMD_TTY_CREATE] = sys_cmd_tty_create,
    [SYSTEM_CMD_TTY_READ_OUT] = sys_cmd_tty_read_out,
    [SYSTEM_CMD_TTY_WRITE_IN] = sys_cmd_tty_write_in,
    [SYSTEM_CMD_TTY_READ_IN] = sys_cmd_tty_read_in,
    [SYSTEM_CMD_SPAWN] = sys_cmd_spawn_process,
    [SYSTEM_CMD_TTY_SET_FG] = sys_cmd_tty_set_fg,
    [SYSTEM_CMD_TTY_GET_FG] = sys_cmd_tty_get_fg,
    [SYSTEM_CMD_TTY_KILL_FG] = sys_cmd_tty_kill_fg,
    [SYSTEM_CMD_TTY_KILL_ALL] = sys_cmd_tty_kill_all,
    [SYSTEM_CMD_TTY_DESTROY] = sys_cmd_tty_destroy,
    [SYSTEM_CMD_EXEC] = sys_cmd_exec_process,
    [SYSTEM_CMD_WAITPID] = sys_cmd_waitpid,
    [SYSTEM_CMD_KILL_SIGNAL] = sys_cmd_kill_signal,
    [SYSTEM_CMD_SIGACTION] = sys_cmd_sigaction,
    [SYSTEM_CMD_SIGPROCMASK] = sys_cmd_sigprocmask,
    [SYSTEM_CMD_SIGPENDING] = sys_cmd_sigpending,
    [SYSTEM_CMD_GET_ELF_METADATA] = sys_cmd_get_elf_metadata,
    [SYSTEM_CMD_GET_ELF_PRIMARY_IMAGE] = sys_cmd_get_elf_primary_image,
    [SYSTEM_CMD_TTY_GET_ID] = sys_cmd_tty_get_id,
    [SYSTEM_CMD_SET_FS_BASE] = sys_cmd_set_fs_base,
    [SYSTEM_CMD_FORK] = sys_cmd_fork_process,
    [SYSTEM_CMD_DISK_GET_COUNT] = sys_cmd_disk_get_count,
    [SYSTEM_CMD_DISK_GET_INFO] = sys_cmd_disk_get_info,
    [SYSTEM_CMD_DISK_WRITE_GPT] = sys_cmd_disk_write_gpt,
    [SYSTEM_CMD_DISK_WRITE_MBR] = sys_cmd_disk_write_mbr,
    [SYSTEM_CMD_DISK_MKFS_FAT32] = sys_cmd_disk_mkfs_fat32,
    [SYSTEM_CMD_DISK_MOUNT] = sys_cmd_disk_mount,
    [SYSTEM_CMD_DISK_UMOUNT] = sys_cmd_disk_umount,
    [SYSTEM_CMD_DISK_RESCAN] = sys_cmd_disk_rescan,
    [SYSTEM_CMD_DISK_REPLACE_KERNEL] = sys_cmd_disk_replace_kernel,
    [SYSTEM_CMD_DISK_SYNC] = sys_cmd_disk_sync,
    [SYSTEM_CMD_PTY_CREATE] = sys_cmd_pty_create,
    [SYSTEM_CMD_PTY_DESTROY] = sys_cmd_pty_destroy,
    [SYSTEM_CMD_GET_PID] = sys_cmd_get_pid,
};

static uint64_t sys_cmd_get_pid(const syscall_args_t *args) {
  (void)args;
  process_t *proc = process_get_current();
  if (!proc) return (uint64_t)-1;
  return (uint64_t)proc->pid;
}

static uint64_t handle_sys_write(const syscall_args_t *args) {
  extern void cmd_write_len(const char *str, size_t len);
  process_t *proc = process_get_current();
  int fd = (int)args->arg1;
  const char *buf = (const char *)args->arg2;
  size_t len = (size_t)args->arg3;

  if (proc && fd >= 0 && fd < MAX_PROCESS_FDS && proc->fds[fd]) {
    syscall_args_t fs_args = *args;
    fs_args.arg2 = args->arg1; // fd
    fs_args.arg3 = args->arg2; // buf
    fs_args.arg4 = args->arg3; // len
    return fs_cmd_write(&fs_args);
  }

  if (!proc || !proc->is_user) {
    cmd_write_len(buf, len);
    return len;
  }
  if (proc->is_terminal_proc) {
    if (proc->tty_id >= 0) {
      tty_write_output(proc->tty_id, buf, len);
      return len;
    }
    cmd_write_len(buf, len);
    return len;
  }
  return len;
}

static uint64_t handle_sys_fs(const syscall_args_t *args) {
  int cmd = (int)args->arg1;
  if (cmd >= 0 && cmd < FS_CMD_TABLE_SIZE && fs_cmd_table[cmd]) {
    return fs_cmd_table[cmd](args);
  }
  return 0;
}

static uint64_t handle_sys_system(const syscall_args_t *args) {
  int cmd = (int)args->arg1;
  if (cmd >= 0 && cmd < SYS_CMD_TABLE_SIZE && sys_cmd_table[cmd]) {
    return sys_cmd_table[cmd](args);
  }
  return -1;
}

static uint64_t handle_debug_serial_write(const syscall_args_t *args) {
  extern void serial_write(const char *str);
  serial_write((const char *)args->arg2);
  return 0;
}

static uint64_t handle_sys_sbrk(const syscall_args_t *args) {
  int incr = (int)args->arg1;
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user)
    return (uint64_t)-1;

  uint64_t old_end = proc->heap_end;
  if (incr == 0)
    return old_end;

  uint64_t new_end = old_end + incr;

  if (incr > 0) {
    uint64_t start_page = (old_end + 0xFFF) & ~0xFFF;
    uint64_t end_page = (new_end + 0xFFF) & ~0xFFF;

    if (end_page > start_page) {
      uint64_t total_size = end_page - start_page;
      void *phys_block = kmalloc_aligned(total_size, 4096);
      if (!phys_block)
        return (uint64_t)-1; // Out of memory

      memset(phys_block, 0, total_size);

      if (proc->sbrk_allocation_count < 64) {
        proc->sbrk_allocations[proc->sbrk_allocation_count++] = phys_block;
      }

      uint64_t phys_addr = (uint64_t)phys_block;
      for (uint64_t page = start_page; page < end_page; page += 4096) {
        paging_map_page(proc->pml4_phys, page, v2p(phys_addr),
                        0x07); // PT_PRESENT | PT_RW | PT_USER
        phys_addr += 4096;
      }
      proc->used_memory += (end_page - start_page);
    }
  }

  proc->heap_end = new_end;
  return old_end;
}

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

static uint64_t handle_sys_kill(const syscall_args_t *args) {
  (void)args;
  return 0;
}

static uint64_t handle_sys_mmap(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user)
    return (uint64_t)MAP_FAILED;

  uint64_t addr = args->arg1;
  uint64_t length = args->arg2;
  int prot = (int)args->arg3;
  int flags = (int)args->arg4;
  int fd = (int)args->arg5;
  uint64_t offset = args->arg6;
  (void)offset;

  if (length == 0)
    return (uint64_t)MAP_FAILED;
  uint64_t aligned_len = (length + 4095) & ~4095ULL;

  uint64_t virt_addr = addr;
  if (virt_addr == 0) {
    virt_addr = proc->mmap_current;
    proc->mmap_current += aligned_len;
  }

  uint64_t pt_flags = PT_PRESENT | PT_USER;
  if (prot & PROT_WRITE)
    pt_flags |= PT_RW;

  if (flags & MAP_ANONYMOUS) {
    // Allocate physical memory for anonymous mapping
    void *phys_block = kmalloc_aligned(aligned_len, 4096);
    if (!phys_block)
      return (uint64_t)MAP_FAILED;
    memset(phys_block, 0, aligned_len);

    // Track the allocation in proc
    if (proc->mmap_allocation_count < 16) {
      proc->mmap_allocations[proc->mmap_allocation_count++] = phys_block;
    }

    uint64_t phys_addr = v2p((uint64_t)phys_block);
    for (uint64_t off = 0; off < aligned_len; off += 4096) {
      paging_map_page(proc->pml4_phys, virt_addr + off, phys_addr + off,
                      pt_flags);
    }
    return virt_addr;
  }

  // File-backed mapping
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return (uint64_t)MAP_FAILED;
  if (proc->fd_kind[fd] != PROC_FD_KIND_FILE)
    return (uint64_t)MAP_FAILED;

  process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
  if (!ref || !ref->file)
    return (uint64_t)MAP_FAILED;
  vfs_file_t *file = ref->file;

  if (file->is_device && file->device_type == DEVICE_TYPE_FRAMEBUFFER) {
    framebuffer_info_t fb = graphics_get_fb_params();
    if (!fb.address)
      return (uint64_t)MAP_FAILED;

    uint64_t phys_addr = v2p((uint64_t)fb.address);
    uint64_t fb_flags = pt_flags | PT_CACHE_DISABLE | PT_WRITE_THROUGH;
    for (uint64_t off = 0; off < aligned_len; off += 4096) {
      paging_map_page(proc->pml4_phys, virt_addr + off, phys_addr + off,
                      fb_flags);
    }
    return virt_addr;
  }

  if (file->is_device && file->device_type == DEVICE_TYPE_SHM) {
    typedef struct shm_segment shm_segment_t;
    extern int shm_allocate(shm_segment_t *seg, size_t size);
    extern void shm_ref(shm_segment_t *seg);
    shm_segment_t *seg = (shm_segment_t *)file->fs_handle;
    if (!seg)
      return (uint64_t)MAP_FAILED;

    // Ensure segment has enough pages for the requested mapping size
    if ((uint64_t)seg->page_count * 4096 < aligned_len) {
      if (shm_allocate(seg, aligned_len) < 0)
        return (uint64_t)MAP_FAILED;
    }

    // Keep the segment alive after the file descriptor is closed.
    // Without this, close(shm_fd) after mmap drops refcount to 0,
    // freeing backing pages while they are still mapped into userspace.
    shm_ref(seg);

    // Track the SHM mapping in proc
    if (proc->shm_mapping_count < 32) {
      proc->shm_mappings[proc->shm_mapping_count].addr = virt_addr;
      proc->shm_mappings[proc->shm_mapping_count].length = aligned_len;
      proc->shm_mappings[proc->shm_mapping_count].seg = (void*)seg;
      proc->shm_mapping_count++;
    }

    // Map pages covering the requested length
    uint32_t pages_to_map = aligned_len / 4096;
    for (uint32_t i = 0; i < pages_to_map; i++) {
      paging_map_page(proc->pml4_phys, virt_addr + i * 4096, seg->phys_pages[i],
                      pt_flags);
    }
    return virt_addr;
  }

  return (uint64_t)MAP_FAILED;
}

static uint64_t handle_sys_munmap(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user)
    return (uint64_t)-1;

  uint64_t addr = args->arg1;
  uint64_t length = args->arg2;

  if (length == 0)
    return 0;
  uint64_t aligned_len = (length + 4095) & ~4095ULL;

  for (uint64_t off = 0; off < aligned_len; off += 4096) {
    paging_unmap_page(proc->pml4_phys, addr + off);
  }

  // Find and release the SHM mapping
  for (uint32_t i = 0; i < proc->shm_mapping_count; i++) {
    if (proc->shm_mappings[i].addr == addr) {
      if (proc->shm_mappings[i].seg) {
        shm_unref((shm_segment_t *)proc->shm_mappings[i].seg);
      }
      // Remove from list by shifting remaining
      for (uint32_t j = i; j < proc->shm_mapping_count - 1; j++) {
        proc->shm_mappings[j] = proc->shm_mappings[j + 1];
      }
      proc->shm_mapping_count--;
      break;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Futex implementation
// ---------------------------------------------------------------------------

#define FUTEX_BUCKETS 64

typedef struct futex_waiter {
  uint32_t *uaddr; // userspace address being waited on
  process_t *proc; // waiting process
  struct futex_waiter *next;
} futex_waiter_t;

typedef struct {
  futex_waiter_t *head;
  spinlock_t lock;
} futex_bucket_t;

static futex_bucket_t g_futex_buckets[FUTEX_BUCKETS];
static bool g_futex_initialized = false;

static void futex_init(void) {
  for (int i = 0; i < FUTEX_BUCKETS; i++) {
    g_futex_buckets[i].head = NULL;
    g_futex_buckets[i].lock = SPINLOCK_INIT;
  }
  g_futex_initialized = true;
}

static inline futex_bucket_t *futex_bucket(uint32_t *uaddr) {
  if (!g_futex_initialized)
    futex_init();
  uintptr_t key = (uintptr_t)uaddr >> 2;
  return &g_futex_buckets[key & (FUTEX_BUCKETS - 1)];
}

/* Public kernel API, usable from sysdep test drivers */
int kernel_futex_wait(uint32_t *uaddr, uint32_t expected) {
  futex_bucket_t *b = futex_bucket(uaddr);
  process_t *proc = process_get_current();
  if (!proc)
    return -1;

  uint64_t flags = spinlock_acquire_irqsave(&b->lock);

  /* Atomically verify the value hasn't changed */
  if (*uaddr != expected) {
    spinlock_release_irqrestore(&b->lock, flags);
    return -11; /* EAGAIN */
  }

  futex_waiter_t waiter;
  waiter.uaddr = uaddr;
  waiter.proc = proc;
  waiter.next = b->head;
  b->head = &waiter;

  proc->state = PROC_STATE_BLOCKED;
  spinlock_release_irqrestore(&b->lock, flags);
  /* Caller (handle_sys_futex) must trigger a reschedule */
  return 0;
}

int kernel_futex_wake(uint32_t *uaddr, int count) {
  futex_bucket_t *b = futex_bucket(uaddr);
  int woken = 0;

  uint64_t flags = spinlock_acquire_irqsave(&b->lock);

  futex_waiter_t **pprev = &b->head;
  futex_waiter_t *cur = b->head;
  while (cur && woken < count) {
    if (cur->uaddr == uaddr) {
      *pprev = cur->next; /* unlink */
      if (cur->proc) {
        cur->proc->state = PROC_STATE_RUNNING;
      }
      woken++;
      cur = *pprev; /* continue from same position */
    } else {
      pprev = &cur->next;
      cur = cur->next;
    }
  }

  spinlock_release_irqrestore(&b->lock, flags);
  return woken;
}

/*
 * Syscall handler: SYS_FUTEX
 *   arg1 = uint32_t *uaddr
 *   arg2 = int op   (FUTEX_WAIT=0 or FUTEX_WAKE=1)
 *   arg3 = uint32_t val  (expected value for WAIT, max wakers for WAKE)
 */
static uint64_t handle_sys_futex(const syscall_args_t *args) {
  uint32_t *uaddr = (uint32_t *)args->arg1;
  int op = (int)args->arg2;
  uint32_t val = (uint32_t)args->arg3;

  if (!uaddr)
    return (uint64_t)-1;

  if (op == FUTEX_WAIT) {
    /* kernel_futex_wait sets proc->state = BLOCKED;
       the caller (syscall_handler_c) must then reschedule. */
    int rc = kernel_futex_wait(uaddr, val);
    return (uint64_t)rc;
  }

  if (op == FUTEX_WAKE) {
    int woken = kernel_futex_wake(uaddr, (int)val);
    return (uint64_t)woken;
  }

  return (uint64_t)-1; /* ENOSYS for unknown ops */
}

#define SYSCALL_TABLE_SIZE 14
static const syscall_handler_fn syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_WRITE] = handle_sys_write, [SYS_FS] = handle_sys_fs,
    [5] = handle_sys_system,        [8] = handle_debug_serial_write,
    [9] = handle_sys_sbrk,          [10] = handle_sys_kill,
    [SYS_MMAP] = handle_sys_mmap,   [SYS_MUNMAP] = handle_sys_munmap,
    [SYS_FUTEX] = handle_sys_futex,
};

static uint64_t syscall_handler_inner(registers_t *regs) {
  uint64_t syscall_num = regs->rax;

  syscall_args_t args = {
      .regs = regs,
      .arg1 = regs->rdi,
      .arg2 = regs->rsi,
      .arg3 = regs->rdx,
      .arg4 = regs->r10,
      .arg5 = regs->r8,
      .arg6 = regs->r9,
  };

  if (syscall_num < SYSCALL_TABLE_SIZE && syscall_table[syscall_num]) {
    return syscall_table[syscall_num](&args);
  }

  return 0;
}

static uint64_t syscall_maybe_deliver_signal(registers_t *regs) {
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user)
    return (uint64_t)regs;

  uint64_t pending = proc->signal_pending & ~proc->signal_mask;
  if (!pending)
    return (uint64_t)regs;

  int sig = -1;
  for (int i = 1; i < MAX_SIGNALS; i++) {
    if (pending & (1ULL << (uint32_t)i)) {
      sig = i;
      break;
    }
  }
  if (sig < 0)
    return (uint64_t)regs;

  proc->signal_pending &= ~(1ULL << (uint32_t)sig);
  uint64_t handler = proc->signal_handlers[sig];
  int flags = proc->signal_action_flags[sig];

  if (handler == 1) {
    return (uint64_t)regs;
  }

  if (handler == 0 || sig == 9) {
    process_terminate_with_status(proc, 128 + sig);
    return process_schedule((uint64_t)regs);
  }

  if (flags & SA_RESETHAND) {
    proc->signal_handlers[sig] = 0;
    proc->signal_action_mask[sig] = 0;
    proc->signal_action_flags[sig] = 0;
  }
  /* Validate handler is a user-space address and the user stack
   * looks sane before writing into user memory. Reject/terminate
   * the process if the handler or stack pointer is invalid to
   * avoid corrupting kernel stack / descriptor frames. */
  const uint64_t KERNEL_BASE = 0xFFFFFFFF80000000ULL;
  if (handler == 1) {
    return (uint64_t)regs;
  }
  /* Handler must be in user-space (not in kernel direct map). */
  if (handler >= KERNEL_BASE) {
    process_terminate_with_status(proc, 128 + 11); /* SIGSEGV */
    return (uint64_t)regs;
  }

  uint64_t new_rsp = regs->rsp - sizeof(uint64_t);
  if (new_rsp >= KERNEL_BASE) {
    process_terminate_with_status(proc, 128 + 11); /* SIGSEGV */
    return (uint64_t)regs;
  }
  /* Ensure the target user address is mapped in the process page tables
   * and write to the underlying physical frame via p2v(). This avoids
   * accidentally writing into kernel memory if regs->rsp was corrupted
   * or pointed at an unmapped address. */
  uint64_t phys = paging_virt2phys(proc->pml4_phys, new_rsp);
  if (!phys) {
    process_terminate_with_status(proc, 128 + 11); /* SIGSEGV */
    return (uint64_t)regs;
  }
  uint64_t *target = (uint64_t *)p2v(phys);
  *target = regs->rip;
  regs->rsp = new_rsp;
  regs->rip = handler;
  regs->rdi = (uint64_t)sig;
  return (uint64_t)regs;
}

uint64_t syscall_handler_c(registers_t *regs) {
  uint64_t syscall_num = regs->rax;

  // Check for context-switching syscalls
  if (syscall_num == 0 || syscall_num == 60) { // EXIT
    int status = (int)regs->rdi;
    return process_terminate_current_with_status((status & 0xff) << 8, (uint64_t)regs);
  }

  if (syscall_num == 10) { // KILL
    uint32_t target_pid = (uint32_t)regs->rdi;
    process_t *current = process_get_current();
    if (target_pid == 0) {
      // Protect kernel process
      regs->rax = -1;
      return (uint64_t)regs;
    }
    if (target_pid == 0xFFFFFFFF || target_pid == current->pid) {
      return process_terminate_current((uint64_t)regs);
    } else {
      process_t *target = process_get_by_pid(target_pid);
      if (target) {
        process_terminate(target);
      }
      regs->rax = 0;
      return (uint64_t)regs;
    }
  }

  if (syscall_num == SYS_SYSTEM && regs->rdi == SYSTEM_CMD_YIELD) {
    extern uint64_t process_schedule(uint64_t current_rsp);
    regs->rax = 0;
    return process_schedule((uint64_t)regs);
  }

  if (syscall_num == SYS_SYSTEM && regs->rdi == SYSTEM_CMD_SLEEP) {
    uint32_t ms = (uint32_t)regs->rsi;
    process_t *proc = process_get_current();
    extern uint32_t get_ticks(void);
    uint32_t ticks = ms / 16;
    if (ticks == 0 && ms > 0)
      ticks = 1;
    proc->sleep_until = get_ticks() + ticks;
    regs->rax = 0;
    return process_schedule((uint64_t)regs);
  }

  // Normal syscalls
  regs->rax = syscall_handler_inner(regs);

  if (syscall_num == SYS_SYSTEM && regs->rdi == SYSTEM_CMD_WAITPID &&
      regs->rax == (uint64_t)-2) {
    regs->rax = 0;
    return process_schedule((uint64_t)regs);
  }

  if (syscall_num == SYS_FS && regs->rax == (uint64_t)-2) {
    regs->rax = -2;
    uint64_t ret = process_schedule((uint64_t)regs);
    poll_cleanup(process_get_current());
    return ret;
  }

  /* FUTEX_WAIT: if the process was just blocked, reschedule */
  if (syscall_num == SYS_FUTEX && regs->rsi == FUTEX_WAIT && regs->rax == 0) {
    process_t *proc = process_get_current();
    if (proc && proc->state == PROC_STATE_BLOCKED) {
      return process_schedule((uint64_t)regs);
    }
  }

  return syscall_maybe_deliver_signal(regs);
}
