// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See
// LICENSE file for details. This header needs to maintain in any file it is
// present in, as per the GPL license terms.
#include "syscall.h"
#include "gdt.h"
#include "slab.h"
#include "process.h"
#include "vfs.h"
#include "shm.h"

#include "cmd.h"
#include "disk.h"
#include "ext4fs.h"
#include "fat32.h"
#include "graphics.h"
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
#include <string.h>

#define SPAWN_FLAG_TERMINAL 0x1
#define SPAWN_FLAG_INHERIT_TTY 0x2
#define SPAWN_FLAG_TTY_ID 0x4

#define MSR_FS_BASE 0xC0000100

static bool is_valid_user_ptr(const void *ptr, size_t size) {
  uint64_t addr = (uint64_t)ptr;
  if (!ptr || addr < 0x1000 || addr >= 0xFFFF800000000000ULL) return false;
  if (addr + size < addr || addr + size >= 0xFFFF800000000000ULL) return false;

  process_t *proc = process_get_current();
  if (!proc || !proc->pml4_phys) return false;

  uint64_t page_start = addr & ~0xFFFULL;
  uint64_t page_end = (addr + (size > 0 ? size - 1 : 0)) & ~0xFFFULL;
  for (uint64_t p = page_start; p <= page_end; p += 4096) {
    if (paging_virt2phys(proc->pml4_phys, p) == 0) {
      return false;
    }
    if (p == page_end) break;
  }
  return true;
}

extern void isr128_wrapper(void);
extern void *kmalloc(size_t size);

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
static void fs_pipe_drop_reader(process_fd_pipe_t **pipe);
static void fs_pipe_drop_writer(process_fd_pipe_t **pipe);
static int fs_copy_unix_path(const void *addr, uint64_t addrlen, char *path_out,
                             size_t path_out_size);
static uint64_t fs_cmd_unix_socket_create(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_bind(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_listen(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_accept(const syscall_args_t *args);
static uint64_t fs_cmd_unix_socket_connect(const syscall_args_t *args);

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

static void fs_pipe_drop_reader(process_fd_pipe_t **pipe) {
  if (!pipe || !*pipe)
    return;

  process_fd_pipe_t *p = *pipe;

  p->readers--;
  if (p->readers <= 0 && p->writers <= 0) {
    kfree_null(*pipe);
  }
}

static void fs_pipe_drop_writer(process_fd_pipe_t **pipe) {
  if (!pipe || !*pipe)
    return;

  process_fd_pipe_t *p = *pipe;

  p->writers--;
  if (p->readers <= 0 && p->writers <= 0) {
    kfree_null(*pipe);
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



static uint64_t fs_cmd_unix_socket_create(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int domain = (int)args->arg2;
  int type = (int)args->arg3;
  int protocol = (int)args->arg4;

  if (!proc || (domain != 1 && domain != 2 && domain != 10 && domain != 17) || (type != 1 && type != 2 && type != 3))
    return -1;

  int fd = fs_alloc_fd_slot(proc, 0);
  if (fd < 0) return -1;

  process_fd_socket_t *sock = process_socket_create();
  if (!sock) return -1;

  sock->domain = (uint8_t)domain;
  sock->type = (uint8_t)type;
  sock->protocol = (uint8_t)protocol;

  if (domain == AF_UNIX) {
    extern int unix_socket_create(void *sock, int type);
    unix_socket_create(sock, type);
  } else if (domain == AF_PACKET || type == SOCK_RAW) {
    extern void raw_tap_register(void *sock);
    raw_tap_register(sock);
  } else {
    extern int network_is_initialized(void);
    extern int network_init(void);
    if (!network_is_initialized()) {
      network_init();
    }
  }

  proc->fds[fd] = sock;
  proc->fd_kind[fd] = PROC_FD_KIND_SOCKET;
  proc->fd_flags[fd] = O_RDWR;
  return fd;
}

static uint64_t fs_cmd_unix_socket_bind(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  const void *addr = (const void *)args->arg3;
  uint64_t addrlen = args->arg4;

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET || !addr) {
    return -1;
  }
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) return -1;

  if (sock->domain == AF_UNIX) {
    char path[108];
    if (fs_copy_unix_path(addr, addrlen, path, sizeof(path)) < 0) return -1;
    extern int unix_socket_bind(void *sock, const char *path);
    return unix_socket_bind(sock, path);
  } else if (sock->domain == AF_INET6) {
    if (addrlen < 24) return -1;
    uint16_t sin6_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin6_port & 0xFF) << 8) | ((sin6_port >> 8) & 0xFF);
    return network_socket_bind_v6(sock, (const ipv6_address_t *)((const char *)addr + 8), port);
  } else {
    if (addrlen < 8) return -1;
    uint16_t sin_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin_port & 0xFF) << 8) | ((sin_port >> 8) & 0xFF);
    uint32_t ip_val = *(const uint32_t *)((const char *)addr + 4);
    int bind_err = network_socket_bind(sock, ip_val, port);
    if (bind_err < 0) return bind_err;
    sock->is_bound = 1;
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_listen(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  int backlog = (int)args->arg3;

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) return -1;

  if (sock->domain == AF_UNIX) {
    extern int unix_socket_listen(void *sock, int backlog);
    return unix_socket_listen(sock, backlog);
  } else {
    if (network_socket_listen(sock, backlog) < 0) return -1;
    sock->is_listening = 1;
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_connect(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  const void *addr = (const void *)args->arg3;
  uint64_t addrlen = args->arg4;

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET || !addr) {
    return -1;
  }
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) return -1;

  if (sock->domain == AF_UNIX) {
    char path[108];
    if (fs_copy_unix_path(addr, addrlen, path, sizeof(path)) < 0) return -1;
    extern int unix_socket_connect(void *sock, const char *path);
    return unix_socket_connect(sock, path);
  } else if (sock->domain == AF_INET6) {
    if (addrlen < 24) return -1;
    uint16_t sin6_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin6_port & 0xFF) << 8) | ((sin6_port >> 8) & 0xFF);
    return network_socket_connect_v6(sock, (const ipv6_address_t *)((const char *)addr + 8), port);
  } else {
    if (addrlen < 8) return -1;
    uint16_t sin_port = *(const uint16_t *)((const char *)addr + 2);
    uint16_t port = ((sin_port & 0xFF) << 8) | ((sin_port >> 8) & 0xFF);
    uint32_t ip_val = *(const uint32_t *)((const char *)addr + 4);
    if (network_socket_connect(sock, ip_val, port) < 0) return -1;
    sock->is_connected = 1;
    return 0;
  }
}

static uint64_t fs_cmd_unix_socket_accept(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  void *addr = (void *)args->arg3;
  uint64_t *addrlen = (uint64_t *)args->arg4;

  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock || !sock->is_listening)
    return -1;

  if (sock->domain == AF_UNIX) {
    int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
    extern void* unix_socket_accept(void *sock, int nonblock);
    process_fd_socket_t *client = (process_fd_socket_t *)unix_socket_accept(sock, nonblock);
    if (!client) return (uint64_t)-2;

    int newfd = fs_alloc_fd_slot(proc, 0);
    if (newfd < 0) {
      process_socket_release(client);
      return -1;
    }
    proc->fds[newfd] = client;
    proc->fd_kind[newfd] = PROC_FD_KIND_SOCKET;
    proc->fd_flags[newfd] = O_RDWR;
    return newfd;
  } else {
    int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
    while (1) {
      uint64_t flags = spinlock_acquire_irqsave(&sock->lock);
      if (sock->accept_head) {
        accept_queue_entry_t *entry = sock->accept_head;
        sock->accept_head = entry->next;
        if (!sock->accept_head) sock->accept_tail = NULL;
        sock->accept_queue_count--;
        spinlock_release_irqrestore(&sock->lock, flags);

        process_fd_socket_t *client = (process_fd_socket_t *)entry->client_sock;
        kfree_null(entry);

        int newfd = fs_alloc_fd_slot(proc, 0);
        if (newfd < 0) {
          process_socket_release(client);
          return -1;
        }

        proc->fds[newfd] = client;
        proc->fd_kind[newfd] = PROC_FD_KIND_SOCKET;
        proc->fd_flags[newfd] = O_RDWR;

        if (addr && addrlen && *addrlen >= 8) {
          uint8_t *a_bytes = (uint8_t *)addr;
          *(uint16_t *)a_bytes = AF_INET;
          uint16_t remote_port = 0; uint32_t remote_ip = 0;
          extern void network_socket_get_remote_info(void *sock, uint16_t *port, uint32_t *ip);
          network_socket_get_remote_info(client, &remote_port, &remote_ip);
          *(uint16_t *)(a_bytes + 2) = ((remote_port & 0xFF) << 8) | ((remote_port >> 8) & 0xFF);
          *(uint32_t *)(a_bytes + 4) = remote_ip;
        }
        return newfd;
      }
      spinlock_release_irqrestore(&sock->lock, flags);

      if (nonblock) return (uint64_t)-2;
      wait_queue_wait(&sock->accept_waitq);
    }
  }
}


static uint64_t fs_cmd_open(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  const char *path = (const char *)args->arg2;
  const char *mode_arg = (const char *)args->arg3;
  if (!path)
    return (uint64_t)-2; // -ENOENT

  const char *mode = "r";
  if (mode_arg != NULL) {
    if ((uintptr_t)mode_arg == 1) mode = "w";
    else if ((uintptr_t)mode_arg == 2) mode = "w+";
    else if ((uintptr_t)mode_arg > 4096) mode = mode_arg;
  }

  vfs_file_t *vf = vfs_open(path, mode);

  if (!vf) {
    if (mode && (mode[0] == 'r' && !strchr(mode, '+'))) {
      return (uint64_t)-2; // -ENOENT
    }
    return (uint64_t)-5; // -EIO
  }

  process_fd_file_ref_t *ref =
      (process_fd_file_ref_t *)kmalloc(sizeof(process_fd_file_ref_t));
  if (!ref) {
    vfs_close(vf);
    return (uint64_t)-12; // -ENOMEM
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

  kfree_null(ref);
  vfs_close(vf);
  return (uint64_t)-24; // -EMFILE
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
    int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
    if (sock->domain == 1) {
      extern int unix_socket_recv(void *sock, void *data, size_t len, int nonblock, void **out_objs, uint8_t *out_kinds, int *out_flags, int *out_fd_count);
      int ret = unix_socket_recv(sock, buf, len, nonblock, NULL, NULL, NULL, NULL);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    } else {
      extern int network_socket_recv(void *sock, void *buf, size_t max_len, int nonblock);
      int ret = network_socket_recv(sock, buf, len, nonblock);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    }
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
    int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
    if (sock->domain == 1) {
      extern int unix_socket_send(void *sock, const void *data, size_t len, int nonblock, const int *pass_fds, int pass_fd_count, const char *dest_path);
      int ret = unix_socket_send(sock, buf, len, nonblock, NULL, 0, NULL);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    } else {
      extern int network_socket_send(void *sock, const void *data, size_t len, int nonblock);
      int ret = network_socket_send(sock, buf, len, nonblock);
      if (ret == -2)
        return (uint64_t)-2;
      return (uint64_t)ret;
    }
  }

  return -1;
}

static uint64_t fs_cmd_close(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  int fd = (int)args->arg2;
  if (fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd])
    return -1;

  process_close_fd_inner(proc, fd);
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

static void fd_addref(process_t *proc, int fd) {
  if (proc->fd_kind[fd] == PROC_FD_KIND_FILE) {
    process_fd_file_ref_t *ref = (process_fd_file_ref_t *)proc->fds[fd];
    if (ref)
      ref->refs++;
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_READ) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    if (pipe)
      pipe->readers++;
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_PIPE_WRITE) {
    process_fd_pipe_t *pipe = (process_fd_pipe_t *)proc->fds[fd];
    if (pipe)
      pipe->writers++;
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    process_socket_addref((process_fd_socket_t *)proc->fds[fd]);
  }
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
  fd_addref(proc, oldfd);

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
  fd_addref(proc, oldfd);

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

static uint64_t fs_list_common(process_t *proc, const char *path,
                               FAT32_FileInfo *u_entries, int max_entries,
                               int offset) {
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
      strcpy(u_entries[i].name, v_entries[i].name);
      u_entries[i].size = v_entries[i].size;
      u_entries[i].is_directory = v_entries[i].is_directory;
      u_entries[i].start_cluster = v_entries[i].start_cluster;
      u_entries[i].write_date = v_entries[i].write_date;
      u_entries[i].write_time = v_entries[i].write_time;
    }
  }
  kfree_null(v_entries);
  return (uint64_t)count;
}


static uint64_t fs_cmd_list_offset(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  return fs_list_common(proc, (const char *)args->arg2,
                        (FAT32_FileInfo *)args->arg3, (int)args->arg4,
                        (int)args->arg5);
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
  return -1;
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
  size_t size = args->arg3;
  if (!buf || size <= 0)
    return -1;
  size_t len = strlen(proc->cwd);
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
    if (wt->entries[i].h) {
      wait_queue_remove(wt->entries[i].h, &wt->entries[i].entry);
      wt->entries[i].h = NULL;
    }
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
  if (proc) {
    poll_cleanup(proc);
  }

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

    int mask = 0;
    if (pty_is_pty_id(fd)) {
      extern int pty_poll_master(int pty_id, struct poll_table *pt);
      mask = pty_poll_master(fd, pt);
    } else {
      if (fd < 0 || fd >= MAX_PROCESS_FDS)
        continue;
      if (!proc->fds[fd]) {
        fds[i].revents = POLLNVAL;
        ready++;
        continue;
      }

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
        if (sock->is_listening) {
          if (sock->domain == AF_UNIX && sock->unpcb) {
            unpcb_t *unp = (unpcb_t *)sock->unpcb;
            if (pt->qproc)
              pt->qproc(&unp->accept_waitq, pt);
            if (unp->accept_count > 0)
              mask |= POLLIN;
          } else {
            if (pt->qproc)
              pt->qproc(&sock->accept_waitq, pt);
            if (sock->accept_queue_count > 0)
              mask |= POLLIN;
          }
        } else {
          if (pt->qproc) {
            pt->qproc(&sock->rx_sb.waitq, pt);
            pt->qproc(&sock->rx_waitq, pt);
          }
          extern int sockbuf_readable(sockbuf_t *sb);
          if (sockbuf_readable(&sock->rx_sb)) mask |= POLLIN;
          if (sock->domain == AF_UNIX && sock->unpcb) {
            unpcb_t *unp = (unpcb_t *)sock->unpcb;
            if (unp->state == UNP_STATE_CLOSED || (unp->peer == NULL && unp->state == UNP_STATE_CONNECTED)) {
              mask |= (POLLIN | POLLHUP);
            }
          }
          if (sock->tcp_closed) mask |= (POLLIN | POLLHUP);
          if (sock->tcp_connect_error) mask |= POLLERR;
          if (sock->is_connected || sock->type == 2) mask |= POLLOUT;
        }
      }
    } else if (proc->fd_kind[fd] == PROC_FD_KIND_TTY) {
      extern int tty_poll(int tty_id, struct poll_table *pt);
      mask = tty_poll(proc->tty_id, pt);
    }
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
    uint32_t ticks = (uint32_t)timeout;
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
  } else if (proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    if (request == 0x5421 /* FIONBIO */ || request == 0x8004667E) {
      if (!arg) return (uint64_t)-1;
      int on = *(int *)arg;
      if (on) proc->fd_flags[fd] |= O_NONBLOCK;
      else proc->fd_flags[fd] &= ~O_NONBLOCK;
      return 0;
    } else if (request == 0x541B /* FIONREAD */) {
      if (!arg) return (uint64_t)-1;
      *(int *)arg = 0; // return 0 bytes pending by default
      return 0;
    }
    return 0; // Succeed basic socket ioctl queries
  }

  return -1;
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

static uint64_t sys_cmd_clone_process(const syscall_args_t *args) {
  extern process_t *process_create_thread(registers_t *parent_regs, uint64_t entry_point, uint64_t user_sp, uint64_t flags);
  uint64_t entry_point = args->arg1;
  uint64_t user_sp = args->arg2;
  uint64_t flags = args->arg3;
  process_t *child = process_create_thread(args->regs, entry_point, user_sp, flags);
  if (!child)
    return (uint64_t)-1;
  return child->pid;
}

static uint64_t sys_cmd_gettid(const syscall_args_t *args) {
  (void)args;
  return process_get_current_pid();
}

static uint64_t handle_sys_set_tid_address(const syscall_args_t *args) {
  (void)args;
  return process_get_current_pid();
}

static uint64_t handle_sys_exit_group(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  if (proc) {
    process_terminate_with_status(proc, (int)args->arg2);
  }
  return 0;
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

  if (sig == 9 || target->signal_handlers[sig] == 0) {
    process_terminate_with_status(target, 128 + sig);
    return 0;
  }

  if (target->signal_handlers[sig] == 1) {
    return 0;
  }

  target->signal_pending |= (1ULL << (uint32_t)sig);
  if (target->state == PROC_STATE_BLOCKED) {
    target->state = PROC_STATE_RUNNING;
    target->sleep_until = 0;
  }
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
  if (!d)
    return (uint64_t)-1;
  if (d->is_fat32) {
    void *vol = fat32_mount_volume(d);
    if (!vol)
      return (uint64_t)-1;
    if (!vfs_mount(mountpoint, devname, "fat32", fat32_get_realfs_ops(), vol))
      return (uint64_t)-1;
    return 0;
  }
  // Try ext4
  void *vol = ext4fs_mount_volume(d);
  if (!vol)
    return (uint64_t)-1;
  if (!vfs_mount(mountpoint, devname, "ext4", ext4fs_get_ops(), vol))
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
    if (m && m->active && strcmp(m->path, mountpoint) == 0) {
      Disk *d = disk_get_by_name(m->device);
      if (d)
        return (uint64_t)disk_sync(d);
    }
  }
  return (uint64_t)-1;
}




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

  if (!buf || len == 0) return 0;

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
      for (uint64_t page = start_page; page < end_page; page += 4096) {
        void *phys_page = kmalloc_aligned(4096, 4096);
        if (!phys_page)
          return old_end;

        memset(phys_page, 0, 4096);

        if (!paging_map_page(proc->pml4_phys, page, v2p((uint64_t)phys_page), 0x07)) {
          kfree_null(phys_page);
          return old_end;
        }
        proc->used_memory += 4096;
      }
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
    for (uint64_t off = 0; off < aligned_len; off += 4096) {
      void *phys_page = kmalloc_aligned(4096, 4096);
      if (!phys_page)
        return (uint64_t)MAP_FAILED;
      memset(phys_page, 0, 4096);

      if (!paging_map_page(proc->pml4_phys, virt_addr + off, v2p((uint64_t)phys_page),
                      pt_flags)) {
        kfree_null(phys_page);
        return (uint64_t)MAP_FAILED;
      }
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
    framebuffer_info_t fb = graphics_get_fb_backing_params();
    if (!fb.address)
      return (uint64_t)MAP_FAILED;

    uint64_t phys_addr = v2p((uint64_t)fb.address);
    uint64_t fb_flags = pt_flags | PT_WRITE_THROUGH;
    for (uint64_t off = 0; off < aligned_len; off += 4096) {
      if (!paging_map_page(proc->pml4_phys, virt_addr + off, phys_addr + off,
                      fb_flags))
        return (uint64_t)MAP_FAILED;
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
    if (proc->shm_mapping_count >= 64) {
      shm_unref(seg);
      return (uint64_t)MAP_FAILED;
    }
    proc->shm_mappings[proc->shm_mapping_count].addr = virt_addr;
    proc->shm_mappings[proc->shm_mapping_count].length = aligned_len;
    proc->shm_mappings[proc->shm_mapping_count].seg = (void*)seg;
    proc->shm_mapping_count++;

    // Map pages covering the requested length
    uint32_t pages_to_map = aligned_len / 4096;
    for (uint32_t i = 0; i < pages_to_map; i++) {
      if (!paging_map_page(proc->pml4_phys, virt_addr + i * 4096, seg->phys_pages[i],
                      pt_flags))
        return (uint64_t)MAP_FAILED;
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
    uint64_t vaddr = addr + off;
    uint64_t phys = paging_virt2phys(proc->pml4_phys, vaddr);
    if (phys) {
      bool is_shm = false;
      for (uint32_t i = 0; i < proc->shm_mapping_count; i++) {
        if (vaddr >= proc->shm_mappings[i].addr &&
            vaddr < proc->shm_mappings[i].addr + proc->shm_mappings[i].length) {
          is_shm = true;
          break;
        }
      }
      if (!is_shm) {
        void *virt_ptr = (void *)p2v(phys);
        extern bool mm_is_heap_address(void *ptr);
        if (mm_is_heap_address(virt_ptr)) {
          kfree_null(virt_ptr);
        }
      }
    }
    paging_unmap_page(proc->pml4_phys, vaddr);
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

typedef struct futex_waiter_entry futex_waiter_t;

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

  proc->futex_waiter.uaddr = uaddr;
  proc->futex_waiter.proc = (struct process *)proc;
  proc->futex_waiter.next = b->head;
  b->head = (futex_waiter_t *)&proc->futex_waiter;

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
        ((process_t *)cur->proc)->state = PROC_STATE_RUNNING;
      }
      cur->uaddr = NULL;
      cur->next = NULL;
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

  int cmd = op & 0x7F;

  if (cmd == 0 || cmd == 9) { // FUTEX_WAIT or FUTEX_WAIT_BITSET
    int rc = kernel_futex_wait(uaddr, val);
    return (uint64_t)rc;
  }

  if (cmd == 1 || cmd == 10) { // FUTEX_WAKE or FUTEX_WAKE_BITSET
    int woken = kernel_futex_wake(uaddr, (int)val);
    return (uint64_t)woken;
  }

  return 0;
}

// Adapters for flat system calls
static uint64_t handle_sys_read(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fd
  shifted.arg3 = args->arg2; // buf
  shifted.arg4 = args->arg3; // count
  return fs_cmd_read(&shifted);
}

static uint64_t handle_sys_open(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  shifted.arg3 = args->arg2; // mode (string)
  return fs_cmd_open(&shifted);
}

static uint64_t handle_sys_close(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fd
  return fs_cmd_close(&shifted);
}

static uint64_t handle_sys_stat(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  shifted.arg3 = args->arg2; // info
  return fs_cmd_get_info(&shifted);
}

static uint64_t handle_sys_poll(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fds
  shifted.arg3 = args->arg2; // nfds
  shifted.arg4 = args->arg3; // timeout
  uint64_t res = fs_cmd_poll(&shifted);
  while (res == (uint64_t)-2) {
    process_t *proc = process_get_current();
    if (proc && proc->state == PROC_STATE_BLOCKED) {
      return (uint64_t)-2;
    }
    shifted.arg4 = 0;
    res = fs_cmd_poll(&shifted);
  }
  process_t *proc = process_get_current();
  if (proc) {
    poll_cleanup(proc);
  }
  return res;
}

static uint64_t handle_sys_lseek(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fd
  shifted.arg3 = args->arg2; // offset
  shifted.arg4 = args->arg3; // whence
  return fs_cmd_seek(&shifted);
}

static uint64_t handle_sys_rt_sigaction(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // sig
  shifted.arg3 = args->arg2; // act
  shifted.arg4 = args->arg3; // oact
  return sys_cmd_sigaction(&shifted);
}

static uint64_t handle_sys_rt_sigprocmask(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // how
  shifted.arg3 = args->arg2; // set
  shifted.arg4 = args->arg3; // oset
  return sys_cmd_sigprocmask(&shifted);
}

static uint64_t handle_sys_ioctl(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  unsigned long cmd = (unsigned long)args->arg2;
  void *arg = (void *)args->arg3;

  process_t *proc = process_get_current();
  if (proc && fd >= 0 && fd < MAX_PROCESS_FDS && proc->fds[fd] &&
      proc->fd_kind[fd] == PROC_FD_KIND_SOCKET) {
    extern int network_if_ioctl(unsigned long cmd, void *arg);
    int ret = network_if_ioctl(cmd, arg);
    if (ret >= 0) return ret;
  }

  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fd
  shifted.arg3 = args->arg2; // request
  shifted.arg4 = args->arg3; // arg
  return fs_cmd_ioctl(&shifted);
}

static uint64_t handle_sys_fcntl(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // fd
  shifted.arg3 = args->arg2; // cmd
  shifted.arg4 = args->arg3; // val
  return fs_cmd_fcntl(&shifted);
}

static uint64_t handle_sys_pipe(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // pipefd
  return fs_cmd_pipe(&shifted);
}

static uint64_t handle_sys_dup(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // oldfd
  return fs_cmd_dup(&shifted);
}

static uint64_t handle_sys_dup2(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // oldfd
  shifted.arg3 = args->arg2; // newfd
  return fs_cmd_dup2(&shifted);
}

static uint64_t handle_sys_socket(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // domain
  shifted.arg3 = args->arg2; // type
  shifted.arg4 = args->arg3; // protocol
  return fs_cmd_unix_socket_create(&shifted);
}

static uint64_t handle_sys_connect(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // sockfd
  shifted.arg3 = args->arg2; // addr
  shifted.arg4 = args->arg3; // addrlen
  return fs_cmd_unix_socket_connect(&shifted);
}

static uint64_t handle_sys_accept(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // sockfd
  shifted.arg3 = args->arg2; // addr
  shifted.arg4 = args->arg3; // addrlen
  return fs_cmd_unix_socket_accept(&shifted);
}

static uint64_t handle_sys_sendto(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  const void *buf = (const void *)args->arg2;
  size_t len = (size_t)args->arg3;
  int flags = (int)args->arg4;
  const void *dest_addr = (const void *)args->arg5;
  uint64_t addrlen = args->arg6;
  (void)flags;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET) {
    return -1;
  }
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) return -1;

  if (sock->domain == 17) {
    extern int nic_send_packet(const void *data, size_t length);
    return nic_send_packet(buf, len) == 0 ? (uint64_t)len : (uint64_t)-1;
  } else if (sock->domain == 1) {
    char path[108] = {0};
    if (dest_addr && addrlen > 2) {
      fs_copy_unix_path(dest_addr, addrlen, path, sizeof(path));
    }
    int nonblock = (proc->fd_flags[fd] & O_NONBLOCK) ? 1 : 0;
    extern int unix_socket_send(void *sock, const void *data, size_t len, int nonblock, const int *pass_fds, int pass_fd_count, const char *dest_path);
    int ret = unix_socket_send(sock, buf, len, nonblock, NULL, 0, path[0] ? path : NULL);
    if (ret == -2) return (uint64_t)-2;
    return (uint64_t)ret;
  } else if (sock->domain == 2) {
    int nonblock = ((flags & 0x40) || (proc->fd_flags[fd] & O_NONBLOCK)) ? 1 : 0;
    if (sock->type == 1) {
      // SOCK_STREAM (TCP) connected send — dest_addr may be NULL
      extern int network_socket_send(void *sock, const void *data, size_t len, int nonblock);
      int ret = network_socket_send(sock, buf, len, nonblock);
      if (ret == -2) return (uint64_t)-2;
      return (uint64_t)ret;
    } else if (sock->type == 2 || sock->type == 3) {
      if (addrlen < 8 || !dest_addr) return -1;
      uint16_t family = *(const uint16_t *)dest_addr;
      if (family != 2) return -1;
      uint16_t sin_port = *(const uint16_t *)((const char *)dest_addr + 2);
      uint16_t port = ((sin_port & 0xFF) << 8) | ((sin_port >> 8) & 0xFF);
      uint32_t ip_val = *(const uint32_t *)((const char *)dest_addr + 4);

      extern int network_socket_sendto(void *sock, const void *data, size_t len, uint32_t dest_ip, uint16_t dest_port);
      return (uint64_t)network_socket_sendto(sock, buf, len, ip_val, port);
    }
  }
  return -1;
}

static uint64_t handle_sys_recvfrom(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  void *buf = (void *)args->arg2;
  size_t len = (size_t)args->arg3;
  int flags = (int)args->arg4;
  void *src_addr = (void *)args->arg5;
  uint32_t *addrlen_ptr = (uint32_t *)args->arg6;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] ||
      proc->fd_kind[fd] != PROC_FD_KIND_SOCKET) {
    return -1;
  }
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (!sock) return -1;

  int nonblock = ((flags & 0x40) || (proc->fd_flags[fd] & O_NONBLOCK)) ? 1 : 0;

  if (sock->domain == AF_PACKET) {
    extern int network_socket_recvfrom(void *sock, void *buf, size_t max_len, int nonblock, uint32_t *from_ip, uint16_t *from_port);
    int ret = network_socket_recvfrom(sock, buf, len, nonblock, NULL, NULL);
    if (ret == -2) return (uint64_t)-2;
    if (ret >= 0 && src_addr && addrlen_ptr && *addrlen_ptr >= 18) {
      *(uint16_t *)src_addr = AF_PACKET;
      *addrlen_ptr = 18;
    }
    return (uint64_t)ret;
  } else if (sock->domain == AF_UNIX) {
    extern int unix_socket_recv(void *sock, void *data, size_t len, int nonblock, void **out_objs, uint8_t *out_kinds, int *out_flags, int *out_fd_count);
    int ret = unix_socket_recv(sock, buf, len, nonblock, NULL, NULL, NULL, NULL);
    if (ret == -2) return (uint64_t)-2;
    return (uint64_t)ret;
  } else if (sock->domain == AF_INET) {
    if (sock->type == SOCK_STREAM) {
      // SOCK_STREAM (TCP) recv
      extern int network_socket_recv(void *sock, void *buf, size_t len, int nonblock);
      int ret = network_socket_recv(sock, buf, len, nonblock);
      if (ret == -2) return (uint64_t)-2;
      return (uint64_t)ret;
    } else if (sock->type == SOCK_DGRAM || sock->type == SOCK_RAW) {
      uint32_t from_ip = 0;
      uint16_t from_port = 0;
      extern int network_socket_recvfrom(void *sock, void *buf, size_t max_len, int nonblock, uint32_t *from_ip, uint16_t *from_port);
      int ret = network_socket_recvfrom(sock, buf, len, nonblock, &from_ip, &from_port);
      if (ret == -2) {
        return (uint64_t)-2;
      }
      if (ret >= 0 && src_addr && addrlen_ptr && *addrlen_ptr >= 8) {
        *(uint16_t *)src_addr = AF_INET;
        uint16_t sin_port = ((from_port & 0xFF) << 8) | ((from_port >> 8) & 0xFF);
        *(uint16_t *)((char *)src_addr + 2) = sin_port;
        *(uint32_t *)((char *)src_addr + 4) = from_ip;
        *addrlen_ptr = 8;
      }
      return (uint64_t)ret;
    }
  }
  return -1;
}

static uint64_t handle_sys_bind(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // sockfd
  shifted.arg3 = args->arg2; // addr
  shifted.arg4 = args->arg3; // addrlen
  return fs_cmd_unix_socket_bind(&shifted);
}

static uint64_t handle_sys_listen(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // sockfd
  shifted.arg3 = args->arg2; // backlog
  return fs_cmd_unix_socket_listen(&shifted);
}

static uint64_t handle_sys_fork(const syscall_args_t *args) {
  return sys_cmd_fork_process(args);
}

static uint64_t handle_sys_execve(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  shifted.arg3 = args->arg2; // args
  return sys_cmd_exec_process(&shifted);
}

static uint64_t handle_sys_wait4(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // pid
  shifted.arg3 = args->arg2; // status
  shifted.arg4 = args->arg3; // options
  return sys_cmd_waitpid(&shifted);
}

static uint64_t handle_sys_kill(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // pid
  shifted.arg3 = args->arg2; // sig
  return sys_cmd_kill_signal(&shifted);
}

static uint64_t handle_sys_rt_sigpending(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // set
  return sys_cmd_sigpending(&shifted);
}

static uint64_t handle_sys_getcwd(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // buf
  shifted.arg3 = args->arg2; // size
  return fs_cmd_getcwd(&shifted);
}

static uint64_t handle_sys_chdir(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  return fs_cmd_chdir(&shifted);
}

static uint64_t handle_sys_mkdir(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  return fs_cmd_mkdir(&shifted);
}

static uint64_t handle_sys_unlink(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1; // path
  return fs_cmd_delete(&shifted);
}

static uint64_t handle_sys_arch_prctl(const syscall_args_t *args) {
  process_t *proc = process_get_current();
  if (!proc) return (uint64_t)-1;

  if (args->arg1 == 0x1002) { // ARCH_SET_FS
    proc->fs_base = args->arg2;
    wrmsr(MSR_FS_BASE, args->arg2);
    return 0;
  } else if (args->arg1 == 0x1003) { // ARCH_GET_FS
    if (args->arg2) *(uint64_t *)args->arg2 = proc->fs_base;
    return 0;
  }
  return (uint64_t)-1;
}


// BoredOS Custom flat wrappers
static uint64_t handle_sys_list_offset(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  shifted.arg4 = args->arg3;
  shifted.arg5 = args->arg4;
  return fs_cmd_list_offset(&shifted);
}

static uint64_t handle_sys_size(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return fs_cmd_size(&shifted);
}

static uint64_t handle_sys_tell(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return fs_cmd_tell(&shifted);
}

static uint64_t handle_sys_exists(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return fs_cmd_exists(&shifted);
}

static uint64_t handle_sys_fs_statfs(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  return fs_cmd_statfs(&shifted);
}

static uint64_t handle_sys_fs_mount_count(const syscall_args_t *args) {
  return fs_cmd_mount_count(args);
}

static uint64_t handle_sys_fs_mount_info(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  return fs_cmd_mount_info(&shifted);
}

static uint64_t handle_sys_spawn(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  shifted.arg4 = args->arg3;
  shifted.arg5 = args->arg4;
  return sys_cmd_spawn_process(&shifted);
}

static uint64_t handle_sys_disk_get_count(const syscall_args_t *args) {
  return sys_cmd_disk_get_count(args);
}

static uint64_t handle_sys_disk_get_info(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  return sys_cmd_disk_get_info(&shifted);
}

static uint64_t handle_sys_disk_write_gpt(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  shifted.arg4 = args->arg3;
  return sys_cmd_disk_write_gpt(&shifted);
}

static uint64_t handle_sys_disk_write_mbr(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  shifted.arg4 = args->arg3;
  return sys_cmd_disk_write_mbr(&shifted);
}

static uint64_t handle_sys_disk_mkfs_fat32(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  return sys_cmd_disk_mkfs_fat32(&shifted);
}

static uint64_t handle_sys_disk_mount(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  shifted.arg3 = args->arg2;
  return sys_cmd_disk_mount(&shifted);
}

static uint64_t handle_sys_disk_umount(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return sys_cmd_disk_umount(&shifted);
}

static uint64_t handle_sys_disk_sync(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return sys_cmd_disk_sync(&shifted);
}

static uint64_t handle_sys_disk_rescan(const syscall_args_t *args) {
  syscall_args_t shifted = *args;
  shifted.arg2 = args->arg1;
  return sys_cmd_disk_rescan(&shifted);
}



static uint64_t handle_sys_reboot(const syscall_args_t *args) {
  return sys_cmd_reboot(args);
}

static uint64_t handle_sys_set_reaper(const syscall_args_t *args) {
  (void)args;
  extern uint32_t reaper_pid;
  if (reaper_pid != 0) return (uint64_t)-1;
  process_t *proc = process_get_current();
  if (!proc || !proc->is_user) return (uint64_t)-1;
  reaper_pid = proc->pid;
  return 0;
}

struct timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct tms {
  int64_t tms_utime;
  int64_t tms_stime;
  int64_t tms_cutime;
  int64_t tms_cstime;
};

static inline uint64_t rdtsc_time(void) {
  uint32_t low, high;
  asm volatile("rdtsc" : "=a"(low), "=d"(high));
  return ((uint64_t)high << 32) | low;
}

static uint64_t get_time_ns_highres(void) {
  extern volatile uint64_t kernel_ticks;
  static uint64_t last_tick = 0;
  static uint64_t last_tsc = 0;
  static uint64_t cycles_per_ms = 3000000;

  uint64_t cur_tick = kernel_ticks;
  uint64_t cur_tsc = rdtsc_time();

  if (cur_tick != last_tick) {
    uint64_t dt = cur_tick - last_tick;
    uint64_t dc = cur_tsc - last_tsc;
    if (dt > 0 && dc > 0 && dt < 100) {
      cycles_per_ms = dc / (dt * 10);
      if (cycles_per_ms < 100000) cycles_per_ms = 100000;
    }
    last_tick = cur_tick;
    last_tsc = cur_tsc;
  }

  uint64_t ms = cur_tick * 10ULL;
  uint64_t sub_ms_cycles = (cur_tsc >= last_tsc) ? (cur_tsc - last_tsc) : 0;
  uint64_t sub_ms_ns = (sub_ms_cycles * 1000000ULL) / (cycles_per_ms ? cycles_per_ms : 3000000ULL);
  if (sub_ms_ns >= 10000000ULL) sub_ms_ns = 9999999ULL;

  return (ms * 1000000ULL) + sub_ms_ns;
}

static uint64_t handle_sys_clock_gettime(const syscall_args_t *args) {
  struct timespec *tp = (struct timespec *)args->arg2;
  if (!is_valid_user_ptr(tp, sizeof(struct timespec))) return (uint64_t)-14; /* EFAULT */
  uint64_t ns = get_time_ns_highres();
  tp->tv_sec = (int64_t)(ns / 1000000000ULL);
  tp->tv_nsec = (int64_t)(ns % 1000000000ULL);
  return 0;
}

static uint64_t handle_sys_clock_getres(const syscall_args_t *args) {
  struct timespec *tp = (struct timespec *)args->arg2;
  if (is_valid_user_ptr(tp, sizeof(struct timespec))) {
    tp->tv_sec = 0;
    tp->tv_nsec = 1;
  }
  return 0;
}

static uint64_t handle_sys_gettimeofday(const syscall_args_t *args) {
  struct timeval *tv = (struct timeval *)args->arg1;
  if (is_valid_user_ptr(tv, sizeof(struct timeval))) {
    uint64_t ns = get_time_ns_highres();
    tv->tv_sec = (int64_t)(ns / 1000000000ULL);
    tv->tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);
  }
  return 0;
}

static uint64_t handle_sys_times(const syscall_args_t *args) {
  struct tms *buf = (struct tms *)args->arg1;
  extern volatile uint64_t kernel_ticks;
  if (is_valid_user_ptr(buf, sizeof(struct tms))) {
    buf->tms_utime = (int64_t)kernel_ticks;
    buf->tms_stime = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
  }
  return (uint64_t)kernel_ticks;
}

static uint64_t handle_sys_nanosleep(const syscall_args_t *args) {
  struct timespec *req = (struct timespec *)args->arg1;
  if (!is_valid_user_ptr(req, sizeof(struct timespec))) return (uint64_t)-14;
  uint64_t ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)req->tv_nsec / 1000000ULL;
  if (ms == 0 && req->tv_nsec > 0) ms = 1;
  extern uint32_t get_ticks(void);
  uint32_t ticks = (uint32_t)ms;
  if (ticks == 0 && ms > 0) ticks = 1;
  process_t *proc = process_get_current();
  if (proc) {
    proc->sleep_until = get_ticks() + ticks;
    proc->state = PROC_STATE_BLOCKED;
  }
  return 0;
}

static uint64_t handle_sys_sched_yield(const syscall_args_t *args) {
  (void)args;
  return 0;
}

static uint64_t handle_sys_setsockopt(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  int level = (int)args->arg2;
  int optname = (int)args->arg3;
  const void *optval = (const void *)args->arg4;
  size_t optlen = (size_t)args->arg5;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  extern int network_setsockopt(void *s, int level, int optname, const void *optval, size_t optlen);
  return network_setsockopt(sock, level, optname, optval, optlen);
}

static uint64_t handle_sys_getsockopt(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  int level = (int)args->arg2;
  int optname = (int)args->arg3;
  void *optval = (void *)args->arg4;
  size_t *optlen = (size_t *)args->arg5;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  extern int network_getsockopt(void *s, int level, int optname, void *optval, size_t *optlen);
  return network_getsockopt(sock, level, optname, optval, optlen);
}

static uint64_t handle_sys_socketpair(const syscall_args_t *args) {
  int domain = (int)args->arg1;
  int type = (int)args->arg2;
  int protocol = (int)args->arg3;
  int *sv = (int *)args->arg4;
  (void)protocol;

  if (domain != 1 || !sv) return -1;

  process_t *proc = process_get_current();
  if (!proc) return -1;

  int fd1 = fs_alloc_fd_slot(proc, 0);
  if (fd1 < 0) return -1;
  process_fd_socket_t *sock1 = process_socket_create();
  if (!sock1) return -1;
  proc->fds[fd1] = sock1;
  proc->fd_kind[fd1] = PROC_FD_KIND_SOCKET;
  proc->fd_flags[fd1] = O_RDWR;

  int fd2 = fs_alloc_fd_slot(proc, 0);
  if (fd2 < 0) {
    proc->fds[fd1] = NULL;
    process_socket_release(sock1);
    return -1;
  }
  process_fd_socket_t *sock2 = process_socket_create();
  if (!sock2) {
    proc->fds[fd1] = NULL;
    process_socket_release(sock1);
    return -1;
  }
  proc->fds[fd2] = sock2;
  proc->fd_kind[fd2] = PROC_FD_KIND_SOCKET;
  proc->fd_flags[fd2] = O_RDWR;

  extern int unix_socketpair(void *sock1, void *sock2, int type);
  if (unix_socketpair(sock1, sock2, type) < 0) {
    proc->fds[fd1] = NULL; process_socket_release(sock1);
    proc->fds[fd2] = NULL; process_socket_release(sock2);
    return -1;
  }

  sv[0] = fd1;
  sv[1] = fd2;
  return 0;
}

static uint64_t handle_sys_getsockname(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  void *addr = (void *)args->arg2;
  uint32_t *addrlen = (uint32_t *)args->arg3;
  (void)addr; (void)addrlen;
  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;
  return 0;
}

static uint64_t handle_sys_getpeername(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  void *addr = (void *)args->arg2;
  uint32_t *addrlen = (uint32_t *)args->arg3;
  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET)
    return -1;
  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];
  if (sock->domain == AF_INET && addr && addrlen && *addrlen >= 8) {
    uint16_t port = 0; uint32_t ip = 0;
    extern void network_socket_get_remote_info(void *sock, uint16_t *port, uint32_t *ip);
    network_socket_get_remote_info(sock, &port, &ip);
    *(uint16_t *)addr = AF_INET;
    *(uint16_t *)((char *)addr + 2) = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);
    *(uint32_t *)((char *)addr + 4) = ip;
    *addrlen = 8;
    return 0;
  }
  return 0;
}

struct user_iovec {
  void *iov_base;
  size_t iov_len;
};

struct user_msghdr {
  void *msg_name;
  uint32_t msg_namelen;
  struct user_iovec *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
};

struct user_cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

static uint64_t handle_sys_sendmsg(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  const struct user_msghdr *msg = (const struct user_msghdr *)args->arg2;
  int flags = (int)args->arg3;
  (void)flags;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET || !msg)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];

  int pass_fds[16];
  int pass_fd_count = 0;
  if (sock->domain == 1 && msg->msg_control && msg->msg_controllen >= sizeof(struct user_cmsghdr)) {
    const struct user_cmsghdr *cmsg = (const struct user_cmsghdr *)msg->msg_control;
    if (cmsg->cmsg_len >= sizeof(struct user_cmsghdr)) {
      const int *fd_payload = (const int *)((const char *)msg->msg_control + sizeof(struct user_cmsghdr));
      size_t payload_bytes = cmsg->cmsg_len - sizeof(struct user_cmsghdr);
      pass_fd_count = (int)(payload_bytes / sizeof(int));
      if (pass_fd_count > 16) pass_fd_count = 16;
      for (int i = 0; i < pass_fd_count; i++) pass_fds[i] = fd_payload[i];
    }
  }

  size_t total_sent = 0;
  int fds_passed_already = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    if (!msg->msg_iov[i].iov_base || msg->msg_iov[i].iov_len == 0) continue;
    if (sock->domain == 1) {
      extern int unix_socket_send(void *sock, const void *data, size_t len, int nonblock, const int *pass_fds, int pass_fd_count, const char *dest_path);
      int cur_fds_cnt = fds_passed_already ? 0 : pass_fd_count;
      int ret = unix_socket_send(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, 0, cur_fds_cnt > 0 ? pass_fds : NULL, cur_fds_cnt, NULL);
      if (ret > 0) {
        total_sent += ret;
        if (cur_fds_cnt > 0) fds_passed_already = 1;
      }
    } else {
      extern int network_socket_send(void *sock, const void *data, size_t len, int nonblock);
      int ret = network_socket_send(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, 0);
      if (ret > 0) total_sent += ret;
    }
  }
  return (uint64_t)total_sent;
}

static uint64_t handle_sys_recvmsg(const syscall_args_t *args) {
  int fd = (int)args->arg1;
  struct user_msghdr *msg = (struct user_msghdr *)args->arg2;
  int flags = (int)args->arg3;
  (void)flags;

  process_t *proc = process_get_current();
  if (!proc || fd < 0 || fd >= MAX_PROCESS_FDS || !proc->fds[fd] || proc->fd_kind[fd] != PROC_FD_KIND_SOCKET || !msg)
    return -1;

  process_fd_socket_t *sock = (process_fd_socket_t *)proc->fds[fd];

  void *rx_objs[16];
  uint8_t rx_kinds[16];
  int rx_flags[16];
  int rx_fd_count = 0;
  size_t total_recvd = 0;

  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    if (!msg->msg_iov[i].iov_base || msg->msg_iov[i].iov_len == 0) continue;
    if (sock->domain == 1) {
      extern int unix_socket_recv(void *sock, void *data, size_t len, int nonblock, void **out_objs, uint8_t *out_kinds, int *out_flags, int *out_fd_count);
      int ret = unix_socket_recv(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, 0, rx_objs, rx_kinds, rx_flags, &rx_fd_count);
      if (ret > 0) total_recvd += ret;
    } else {
      extern int network_socket_recv(void *sock, void *data, size_t len, int nonblock);
      int ret = network_socket_recv(sock, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len, 0);
      if (ret > 0) total_recvd += ret;
    }
  }

  if (sock->domain == 1 && rx_fd_count > 0 && msg->msg_control && msg->msg_controllen >= sizeof(struct user_cmsghdr)) {
    struct user_cmsghdr *cmsg = (struct user_cmsghdr *)msg->msg_control;
    cmsg->cmsg_level = 1; // SOL_SOCKET
    cmsg->cmsg_type = 1;  // SCM_RIGHTS

    int *fd_payload = (int *)((char *)msg->msg_control + sizeof(struct user_cmsghdr));
    int installed = 0;
    for (int i = 0; i < rx_fd_count; i++) {
      int newfd = fs_alloc_fd_slot(proc, 0);
      if (newfd >= 0) {
        proc->fds[newfd] = rx_objs[i];
        proc->fd_kind[newfd] = rx_kinds[i];
        proc->fd_flags[newfd] = rx_flags[i];
        fd_payload[installed++] = newfd;
      } else {
        if (rx_kinds[i] == PROC_FD_KIND_FILE) {
          process_fd_file_ref_t *ref = (process_fd_file_ref_t *)rx_objs[i];
          if (ref && ref->refs > 0) ref->refs--;
        } else if (rx_kinds[i] == PROC_FD_KIND_SOCKET) {
          process_socket_release((process_fd_socket_t *)rx_objs[i]);
        }
      }
    }
    cmsg->cmsg_len = sizeof(struct user_cmsghdr) + installed * sizeof(int);
    msg->msg_controllen = cmsg->cmsg_len;
  }

  return (uint64_t)total_recvd;
}

static uint64_t handle_sys_shutdown(const syscall_args_t *args) {
  return sys_cmd_shutdown(args);
}

static uint64_t handle_sys_sysctl(const syscall_args_t *args) {
    extern void kernel_get_hostname(char *buf, size_t max_len);
    extern int kernel_set_hostname(const char *name, size_t len);

    const int *name = (const int *)args->arg1;
    unsigned int nlen = (unsigned int)args->arg2;
    void *oldp = (void *)args->arg3;
    size_t *oldlenp = (size_t *)args->arg4;
    const void *newp = (const void *)args->arg5;
    size_t newlen = (size_t)args->arg6;

    if (!name || nlen < 2) return (uint64_t)-1;

    // CTL_KERN = 1, KERN_HOSTNAME = 10
    if (name[0] == 1 && name[1] == 10) {
        if (oldp && oldlenp && *oldlenp > 0) {
            kernel_get_hostname((char *)oldp, *oldlenp);
            *oldlenp = strlen((char *)oldp);
        }
        if (newp && newlen > 0) {
            return (uint64_t)kernel_set_hostname((const char *)newp, newlen);
        }
        return 0;
    }

    return (uint64_t)-1;
}

#define SYSCALL_TABLE_SIZE 351
static const syscall_handler_fn syscall_table[SYSCALL_TABLE_SIZE] = {
    [SYS_READ] = handle_sys_read,
    [SYS_WRITE] = handle_sys_write,
    [SYS_OPEN] = handle_sys_open,
    [SYS_CLOSE] = handle_sys_close,
    [SYS_STAT] = handle_sys_stat,
    [SYS_POLL] = handle_sys_poll,
    [SYS_LSEEK] = handle_sys_lseek,
    [SYS_MMAP] = handle_sys_mmap,
    [SYS_MUNMAP] = handle_sys_munmap,
    [SYS_BRK] = handle_sys_sbrk,
    [SYS_RT_SIGACTION] = handle_sys_rt_sigaction,
    [SYS_RT_SIGPROCMASK] = handle_sys_rt_sigprocmask,
    [SYS_IOCTL] = handle_sys_ioctl,
    [SYS_PIPE] = handle_sys_pipe,
    [SYS_SCHED_YIELD] = handle_sys_sched_yield,
    [SYS_DUP] = handle_sys_dup,
    [SYS_DUP2] = handle_sys_dup2,
    [SYS_NANOSLEEP] = handle_sys_nanosleep,
    [SYS_GETPID] = sys_cmd_get_pid,
    [SYS_SOCKET] = handle_sys_socket,
    [SYS_CONNECT] = handle_sys_connect,
    [SYS_ACCEPT] = handle_sys_accept,
    [SYS_SENDTO] = handle_sys_sendto,
    [SYS_RECVFROM] = handle_sys_recvfrom,
    [SYS_SENDMSG] = handle_sys_sendmsg,
    [SYS_RECVMSG] = handle_sys_recvmsg,
    [SYS_BIND] = handle_sys_bind,
    [SYS_LISTEN] = handle_sys_listen,
    [SYS_GETSOCKNAME] = handle_sys_getsockname,
    [SYS_GETPEERNAME] = handle_sys_getpeername,
    [SYS_SOCKETPAIR] = handle_sys_socketpair,
    [SYS_SETSOCKOPT] = handle_sys_setsockopt,
    [SYS_GETSOCKOPT] = handle_sys_getsockopt,
    [SYS_CLONE] = sys_cmd_clone_process,
    [SYS_FORK] = handle_sys_fork,
    [SYS_EXECVE] = handle_sys_execve,
    [SYS_WAIT4] = handle_sys_wait4,
    [SYS_KILL] = handle_sys_kill,
    [SYS_FCNTL] = handle_sys_fcntl,
    [SYS_RT_SIGPENDING] = handle_sys_rt_sigpending,
    [SYS_GETCWD] = handle_sys_getcwd,
    [SYS_CHDIR] = handle_sys_chdir,
    [SYS_MKDIR] = handle_sys_mkdir,
    [SYS_UNLINK] = handle_sys_unlink,
    [SYS_GETTIMEOFDAY] = handle_sys_gettimeofday,
    [SYS_TIMES] = handle_sys_times,
    [SYS_ARCH_PRCTL] = handle_sys_arch_prctl,
    [SYS_GETTID] = sys_cmd_gettid,
    [SYS_FUTEX] = handle_sys_futex,
    [SYS_SET_TID_ADDRESS] = handle_sys_set_tid_address,
    [SYS_CLOCK_GETTIME] = handle_sys_clock_gettime,
    [SYS_CLOCK_GETRES] = handle_sys_clock_getres,
    [SYS_EXIT_GROUP] = handle_sys_exit_group,

    // Custom BoredOS system calls
    [SYS_LIST_OFFSET] = handle_sys_list_offset,
    [SYS_SIZE] = handle_sys_size,
    [SYS_TELL] = handle_sys_tell,
    [SYS_EXISTS] = handle_sys_exists,
    [SYS_FS_STATFS] = handle_sys_fs_statfs,
    [SYS_FS_MOUNT_COUNT] = handle_sys_fs_mount_count,
    [SYS_FS_MOUNT_INFO] = handle_sys_fs_mount_info,
    [SYS_SPAWN] = handle_sys_spawn,
    [SYS_DISK_GET_COUNT] = handle_sys_disk_get_count,
    [SYS_DISK_GET_INFO] = handle_sys_disk_get_info,
    [SYS_DISK_WRITE_GPT] = handle_sys_disk_write_gpt,
    [SYS_DISK_WRITE_MBR] = handle_sys_disk_write_mbr,
    [SYS_DISK_MKFS_FAT32] = handle_sys_disk_mkfs_fat32,
    [SYS_DISK_MOUNT] = handle_sys_disk_mount,
    [SYS_DISK_UMOUNT] = handle_sys_disk_umount,
    [SYS_DISK_SYNC] = handle_sys_disk_sync,
    [SYS_DISK_RESCAN] = handle_sys_disk_rescan,
    [SYS_REBOOT] = handle_sys_reboot,
    [SYS_SET_REAPER] = handle_sys_set_reaper,
    [SYS_SHUTDOWN] = handle_sys_shutdown,
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
  if (!proc || !proc->is_user || (regs->cs & 0x3) == 0)
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
    return process_terminate_current_with_status(128 + 11, (uint64_t)regs);
  }

  uint64_t new_rsp = regs->rsp - sizeof(uint64_t);
  if (new_rsp >= KERNEL_BASE) {
    return process_terminate_current_with_status(128 + 11, (uint64_t)regs);
  }
  /* Ensure the target user address is mapped in the process page tables
   * and write to the underlying physical frame via p2v(). This avoids
   * accidentally writing into kernel memory if regs->rsp was corrupted
   * or pointed at an unmapped address. */
  uint64_t phys = paging_virt2phys(proc->pml4_phys, new_rsp);
  if (!phys) {
    return process_terminate_current_with_status(128 + 11, (uint64_t)regs);
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
  if (syscall_num == SYS_EXIT) { // EXIT
    int status = (int)regs->rdi;
    return process_terminate_current_with_status((status & 0xff) << 8, (uint64_t)regs);
  }

  // Normal syscalls
  regs->rax = syscall_handler_inner(regs);

  process_t *cur_proc = process_get_current();
  if (cur_proc && cur_proc->kill_pending) {
    return process_terminate_current_with_status(cur_proc->exit_status ? cur_proc->exit_status : 1, (uint64_t)regs);
  }

  if (cur_proc && cur_proc->state == PROC_STATE_BLOCKED) {
    return process_schedule((uint64_t)regs);
  }

  if (syscall_num == SYS_SCHED_YIELD) {
    regs->rax = 0;
    return process_schedule((uint64_t)regs);
  }

  return syscall_maybe_deliver_signal(regs);
}
