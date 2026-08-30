# BoredOS System Call Reference

BoredOS implements a system call interface supporting standard Linux x86_64 ABI syscalls (0 to 299) and custom BoredOS syscalls (300+).

## 1. Standard System Calls (0 - 299)

| Number | Symbol | Arguments | Description |
| :--- | :--- | :--- | :--- |
| 0 | `SYS_READ` | `int fd, void *buf, size_t count` | Read bytes from file descriptor. |
| 1 | `SYS_WRITE` | `int fd, const void *buf, size_t count` | Write bytes to file descriptor. |
| 2 | `SYS_OPEN` | `const char *path, const char *mode` | Open file or device node. |
| 3 | `SYS_CLOSE` | `int fd` | Close file descriptor. |
| 4 | `SYS_STAT` | `const char *path, void *info` | Get file metadata. |
| 7 | `SYS_POLL` | `struct pollfd *fds, nfds_t nfds, int timeout` | Poll file descriptors for I/O readiness. |
| 8 | `SYS_LSEEK` | `int fd, off_t offset, int whence` | Reposition read/write file offset. |
| 9 | `SYS_MMAP` | `void *addr, size_t len, int prot, int flags, int fd, off_t offset` | Map memory pages or files into address space. |
| 10 | `SYS_MPROTECT` | `void *addr, size_t len, int prot` | Change memory protection permissions. |
| 11 | `SYS_MUNMAP` | `void *addr, size_t len` | Unmap memory pages from address space. |
| 12 | `SYS_BRK` | `void *addr` | Change data segment size (heap break). |
| 13 | `SYS_RT_SIGACTION` | `int sig, const struct sigaction *act, struct sigaction *oact` | Examine and change signal actions. |
| 14 | `SYS_RT_SIGPROCMASK` | `int how, const sigset_t *set, sigset_t *oset` | Change blocked signal mask. |
| 16 | `SYS_IOCTL` | `int fd, unsigned long request, void *argp` | Device control operation. |
| 22 | `SYS_PIPE` | `int pipefd[2]` | Create unidirectional pipe pair. |
| 24 | `SYS_SCHED_YIELD` | *none* | Yield remaining CPU timeslice. |
| 32 | `SYS_DUP` | `int oldfd` | Duplicate a file descriptor. |
| 33 | `SYS_DUP2` | `int oldfd, int newfd` | Duplicate a file descriptor to a specified index. |
| 35 | `SYS_NANOSLEEP` | `uint32_t ms` | Sleep for duration in milliseconds. |
| 39 | `SYS_GETPID` | *none* | Get current process ID. |
| 41 | `SYS_SOCKET` | `int domain, int type, int protocol` | Create communication endpoint. |
| 42 | `SYS_CONNECT` | `int fd, const struct sockaddr *addr, socklen_t len` | Connect socket to target address. |
| 43 | `SYS_ACCEPT` | `int fd, struct sockaddr *addr, socklen_t *len` | Accept incoming connection on socket. |
| 44 | `SYS_SENDTO` | `int fd, const void *buf, size_t len, int flags, ...` | Send data on socket. |
| 45 | `SYS_RECVFROM` | `int fd, void *buf, size_t len, int flags, ...` | Receive data from socket. |
| 46 | `SYS_SENDMSG` | `int fd, const struct msghdr *msg, int flags` | Send message over socket. |
| 47 | `SYS_RECVMSG` | `int fd, struct msghdr *msg, int flags` | Receive message from socket. |
| 49 | `SYS_BIND` | `int fd, const struct sockaddr *addr, socklen_t len` | Bind socket to local address/port. |
| 50 | `SYS_LISTEN` | `int fd, int backlog` | Listen for incoming socket connections. |
| 51 | `SYS_GETSOCKNAME` | `int fd, struct sockaddr *addr, socklen_t *len` | Get socket local address. |
| 52 | `SYS_GETPEERNAME` | `int fd, struct sockaddr *addr, socklen_t *len` | Get socket peer address. |
| 53 | `SYS_SOCKETPAIR` | `int domain, int type, int protocol, int sv[2]` | Create pair of connected sockets. |
| 54 | `SYS_SETSOCKOPT` | `int fd, int level, int optname, const void *optval, socklen_t optlen` | Set socket option. |
| 55 | `SYS_GETSOCKOPT` | `int fd, int level, int optname, void *optval, socklen_t *optlen` | Get socket option. |
| 56 | `SYS_CLONE` | `unsigned long flags, void *child_stack, void *ptid, void *ctid, void *tls` | Clone process / thread. |
| 57 | `SYS_FORK` | *none* | Create child process with COW address space. |
| 59 | `SYS_EXECVE` | `const char *path, char *const argv[], char *const envp[]` | Execute ELF binary. |
| 60 | `SYS_EXIT` | `int status` | Terminate current process. |
| 61 | `SYS_WAIT4` | `pid_t pid, int *wstatus, int options, void *rusage` | Wait for child process state change. |
| 62 | `SYS_KILL` | `pid_t pid, int sig` | Send signal to process. |
| 72 | `SYS_FCNTL` | `int fd, int cmd, int val` | Manipulate file descriptor properties. |
| 73 | `SYS_RT_SIGPENDING` | `sigset_t *set` | Check pending signals. |
| 79 | `SYS_GETCWD` | `char *buf, size_t size` | Get current working directory. |
| 80 | `SYS_CHDIR` | `const char *path` | Change current working directory. |
| 83 | `SYS_MKDIR` | `const char *path` | Create directory. |
| 87 | `SYS_UNLINK` | `const char *path` | Delete filesystem node. |
| 96 | `SYS_GETTIMEOFDAY` | `struct timeval *tv, struct timezone *tz` | Get current clock time. |
| 100 | `SYS_TIMES` | `struct tms *buf` | Get process execution times. |
| 158 | `SYS_ARCH_PRCTL` | `int code, unsigned long addr` | Set architecture-specific state (such as `ARCH_SET_FS`). |
| 186 | `SYS_GETTID` | *none* | Get thread ID. |
| 202 | `SYS_FUTEX` | `uint32_t *uaddr, int op, uint32_t val` | Fast userspace locking primitive (`FUTEX_WAIT`, `FUTEX_WAKE`). |
| 218 | `SYS_SET_TID_ADDRESS` | `int *tidptr` | Set clear-child-tid address. |
| 228 | `SYS_CLOCK_GETTIME` | `int clockid, struct timespec *tp` | Read clock timestamp. |
| 229 | `SYS_CLOCK_GETRES` | `int clockid, struct timespec *res` | Get clock resolution. |
| 231 | `SYS_EXIT_GROUP` | `int status` | Terminate all threads in thread group. |

---

## 2. Custom BoredOS System Calls (300+)

| Number | Symbol | Arguments | Description |
| :--- | :--- | :--- | :--- |
| 300 | `SYS_LIST_OFFSET` | `const char *path, void *entries, int max, int offset` | List directory contents at offset. |
| 301 | `SYS_SIZE` | `int fd` | Get total size of file descriptor. |
| 302 | `SYS_TELL` | `int fd` | Get current file offset position. |
| 303 | `SYS_EXISTS` | `const char *path` | Check if path exists in VFS. |
| 304 | `SYS_FS_STATFS` | `const char *path, vfs_statfs_t *stat` | Get filesystem usage statistics. |
| 305 | `SYS_FS_MOUNT_COUNT` | *none* | Get number of active VFS mounts. |
| 306 | `SYS_FS_MOUNT_INFO` | `int index, void *info` | Get mount details by index. |
| 317 | `SYS_SPAWN` | `const char *path, char *const argv[], char *const envp[], uint32_t flags` | Spawn process directly. |
| 318 | `SYS_SET_REAPER` | *none* | Set calling process as child subreaper. |
| 322 | `SYS_DISK_GET_COUNT` | *none* | Get count of detected physical disks. |
| 323 | `SYS_DISK_GET_INFO` | `int index, void *out` | Get disk model, size, and partition information. |
| 327 | `SYS_DISK_MOUNT` | `const char *devname, const char *mountpoint` | Mount block device to VFS path. |
| 328 | `SYS_DISK_UMOUNT` | `const char *mountpoint` | Unmount filesystem. |
| 329 | `SYS_DISK_SYNC` | `const char *mountpoint` | Flush writeback cache for mountpoint. |
| 330 | `SYS_DISK_RESCAN` | `const char *devname` | Rescan partition table on block device. |
| 349 | `SYS_REBOOT` | *none* | Trigger system reboot. |
| 350 | `SYS_SHUTDOWN` | *none* | Power off machine via ACPI. |