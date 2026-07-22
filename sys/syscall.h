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
    SYS_READ = 0,
    SYS_WRITE = 1,
    SYS_OPEN = 2,
    SYS_CLOSE = 3,
    SYS_STAT = 4,
    SYS_POLL = 7,
    SYS_LSEEK = 8,
    SYS_MMAP = 9,
    SYS_MUNMAP = 11,
    SYS_BRK = 12,
    SYS_RT_SIGACTION = 13,
    SYS_RT_SIGPROCMASK = 14,
    SYS_IOCTL = 16,
    SYS_PIPE = 22,
    SYS_SCHED_YIELD = 24,
    SYS_DUP = 32,
    SYS_DUP2 = 33,
    SYS_NANOSLEEP = 35,
    SYS_GETPID = 39,
    SYS_SOCKET = 41,
    SYS_CONNECT = 42,
    SYS_ACCEPT = 43,
    SYS_SENDTO = 44,
    SYS_RECVFROM = 45,
    SYS_BIND = 49,
    SYS_LISTEN = 50,
    SYS_FORK = 57,
    SYS_EXECVE = 59,
    SYS_EXIT = 60,
    SYS_WAIT4 = 61,
    SYS_KILL = 62,
    SYS_FCNTL = 72,
    SYS_RT_SIGPENDING = 73,
    SYS_GETCWD = 79,
    SYS_CHDIR = 80,
    SYS_MKDIR = 83,
    SYS_UNLINK = 87,
    SYS_GETTIMEOFDAY = 96,
    SYS_TIMES = 100,
    SYS_ARCH_PRCTL = 158,
    SYS_FUTEX = 202,
    SYS_CLOCK_GETTIME = 228,
    SYS_CLOCK_GETRES = 229,

    // Custom BoredOS system calls
    SYS_LIST_OFFSET = 300,
    SYS_SIZE = 301,
    SYS_TELL = 302,
    SYS_EXISTS = 303,
    SYS_FS_STATFS = 304,
    SYS_FS_MOUNT_COUNT = 305,
    SYS_FS_MOUNT_INFO = 306,
    SYS_TTY_CREATE = 307,
    SYS_TTY_READ_OUT = 308,
    SYS_TTY_WRITE_IN = 309,
    SYS_TTY_READ_IN = 310,
    SYS_TTY_DESTROY = 311,
    SYS_TTY_SET_FG = 312,
    SYS_TTY_GET_FG = 313,
    SYS_TTY_KILL_FG = 314,
    SYS_TTY_KILL_ALL = 315,
    SYS_TTY_GET_ID = 316,
    SYS_SPAWN = 317,
    SYS_PTY_CREATE = 320,
    SYS_PTY_DESTROY = 321,
    SYS_SET_REAPER = 318,
    SYS_DISK_GET_COUNT = 322,
    SYS_DISK_GET_INFO = 323,
    SYS_DISK_WRITE_GPT = 324,
    SYS_DISK_WRITE_MBR = 325,
    SYS_DISK_MKFS_FAT32 = 326,
    SYS_DISK_MOUNT = 327,
    SYS_DISK_UMOUNT = 328,
    SYS_DISK_SYNC = 329,
    SYS_DISK_RESCAN = 330,
    SYS_REBOOT = 349,
    SYS_SHUTDOWN = 350
} syscall_t;

// Futex operations (mlibc FutexWait/FutexWake)
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

void syscall_init(void);
uint64_t syscall_handler_c(registers_t *regs);
int kernel_futex_wait(uint32_t *uaddr, uint32_t expected);
int kernel_futex_wake(uint32_t *uaddr, int count);

#endif // SYSCALL_H
