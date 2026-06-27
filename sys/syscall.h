// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

typedef struct registers_t registers_t;
// MSRs used for syscalls in x86_64
#define MSR_EFER       0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_COMPAT_STAR 0xC0000083
#define MSR_FMASK      0xC0000084

// Syscall Numbers
typedef enum {
    SYS_WRITE = 1,
    SYS_FS = 4,
    SYS_SYSTEM = 5,
    SYS_MMAP = 11,
    SYS_MUNMAP = 12,
    SYS_FUTEX = 13,
    SYS_EXIT = 60
} syscall_t;

// Futex operations (mlibc FutexWait/FutexWake)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

// FS Commands
typedef enum {
    FS_CMD_OPEN = 1,
    FS_CMD_READ = 2,
    FS_CMD_WRITE = 3,
    FS_CMD_CLOSE = 4,
    FS_CMD_SEEK = 5,
    FS_CMD_TELL = 6,
    FS_CMD_LIST = 7,
    FS_CMD_DELETE = 8,
    FS_CMD_SIZE = 9,
    FS_CMD_MKDIR = 10,
    FS_CMD_EXISTS = 11,
    FS_CMD_GETCWD = 12,
    FS_CMD_CHDIR = 13,
    FS_CMD_GET_INFO = 14,
    FS_CMD_DUP = 15,
    FS_CMD_DUP2 = 16,
    FS_CMD_PIPE = 17,
    FS_CMD_FCNTL = 18,
    FS_CMD_STATFS = 19,
    FS_CMD_MOUNT_COUNT = 20,
    FS_CMD_MOUNT_INFO = 21,
    FS_CMD_POLL = 22,
    FS_CMD_IOCTL = 24,
    FS_CMD_UNIX_SOCKET_CREATE = 25,
    FS_CMD_UNIX_SOCKET_BIND = 26,
    FS_CMD_UNIX_SOCKET_LISTEN = 27,
    FS_CMD_UNIX_SOCKET_ACCEPT = 28,
    FS_CMD_UNIX_SOCKET_CONNECT = 29,
    FS_CMD_UNIX_SOCKET_SEND = 30,
    FS_CMD_UNIX_SOCKET_RECV = 31,
    FS_CMD_UNIX_SOCKET_CLOSE = 32,
    FS_CMD_UNIX_SOCKET_UNLINK = 33,
    FS_CMD_LIST_OFFSET = 34
} fs_cmd_t;

typedef enum {
    SYSTEM_CMD_RTC_GET = 11,
    SYSTEM_CMD_REBOOT = 12,
    SYSTEM_CMD_SHUTDOWN = 13,
    SYSTEM_CMD_GET_MEM_INFO = 15,
    SYSTEM_CMD_GET_TICKS = 16,
    SYSTEM_CMD_PCI_LIST = 17,
    SYSTEM_CMD_UDP_SEND = 22,
    SYSTEM_CMD_ICMP_PING = 26,
    SYSTEM_CMD_SET_TEXT_COLOR = 29,
    SYSTEM_CMD_RTC_SET = 32,
    SYSTEM_CMD_TCP_CONNECT = 33,
    SYSTEM_CMD_TCP_SEND = 34,
    SYSTEM_CMD_TCP_RECV = 35,
    SYSTEM_CMD_TCP_CLOSE = 36,
    SYSTEM_CMD_DNS_LOOKUP = 37,
    SYSTEM_CMD_NET_UNLOCK = 39,
    SYSTEM_CMD_SET_RAW_MODE = 41,
    SYSTEM_CMD_TCP_RECV_NB = 42,
    SYSTEM_CMD_YIELD = 43,
    SYSTEM_CMD_TCP_LISTEN = 44,
    SYSTEM_CMD_TCP_ACCEPT = 45,
    SYSTEM_CMD_SLEEP = 46,
    SYSTEM_CMD_SET_KEYBOARD_LAYOUT = 49,
    SYSTEM_CMD_PARALLEL_RUN = 50,
    SYSTEM_CMD_GET_KEYBOARD_LAYOUT = 51,
    SYSTEM_CMD_TTY_CREATE = 60,
    SYSTEM_CMD_TTY_READ_OUT = 61,
    SYSTEM_CMD_TTY_WRITE_IN = 62,
    SYSTEM_CMD_TTY_READ_IN = 63,
    SYSTEM_CMD_SPAWN = 64,
    SYSTEM_CMD_TTY_SET_FG = 65,
    SYSTEM_CMD_TTY_GET_FG = 66,
    SYSTEM_CMD_TTY_KILL_FG = 67,
    SYSTEM_CMD_TTY_KILL_ALL = 68,
    SYSTEM_CMD_TTY_DESTROY = 69,
    SYSTEM_CMD_EXEC = 70,
    SYSTEM_CMD_WAITPID = 71,
    SYSTEM_CMD_KILL_SIGNAL = 72,
    SYSTEM_CMD_SIGACTION = 73,
    SYSTEM_CMD_SIGPROCMASK = 74,
    SYSTEM_CMD_SIGPENDING = 75,
    SYSTEM_CMD_GET_ELF_METADATA = 76,
    SYSTEM_CMD_GET_ELF_PRIMARY_IMAGE = 77,
    SYSTEM_CMD_TTY_GET_ID = 78,
    SYSTEM_CMD_SET_FS_BASE = 79,
    SYSTEM_CMD_FORK = 80,
    SYSTEM_CMD_PTY_CREATE = 82,
    SYSTEM_CMD_PTY_DESTROY = 83
    ,SYSTEM_CMD_GET_PID = 84
} system_cmd_t;

void syscall_init(void);
uint64_t syscall_handler_c(registers_t *regs);
int kernel_futex_wait(uint32_t *uaddr, uint32_t expected);
int kernel_futex_wake(uint32_t *uaddr, int count);

#endif // SYSCALL_H
