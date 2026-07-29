# BoredOS System Call Reference

BoredOS implements a standard, flat system call interface. It provides a direct POSIX-compatible subset (numbers 0 to 202) alongside specialized custom system calls (numbers 300+) to support filesystems, virtual terminals, pseudo-terminals, disks, and system actions.

> [!NOTE]
> The historical multiplexed system calls (`SYS_FS = 4` and `SYS_SYSTEM = 5`) and command sub-IDs have been completely removed. Standard filesystem, device control, and system state operations are now routed directly through standard syscalls or mapped cleanly inside the Virtual File System (VFS).

---

## 1. Direct POSIX-Compatible System Calls (0 - 299)

These system calls conform to standard Linux x86_64 ABI layouts, allowing standard C libraries and applications to run with minimal porting:

| Number | Symbol / Enum | Arguments | Description |
| :--- | :--- | :--- | :--- |
| **0** | `SYS_READ` | `int fd, void *buf, size_t count` | Reads bytes from file descriptor `fd` into `buf`. |
| **1** | `SYS_WRITE` | `int fd, const void *buf, size_t count` | Writes bytes from `buf` to file descriptor `fd`. |
| **2** | `SYS_OPEN` | `const char *path, const char *mode` | Opens file/device at `path` using standard C mode string. |
| **3** | `SYS_CLOSE` | `int fd` | Closes file descriptor `fd` and frees resources. |
| **4** | `SYS_STAT` | `const char *path, FAT32_FileInfo *info` | Retrieves metadata of target filesystem node. |
| **7** | `SYS_POLL` | `struct pollfd *fds, nfds_t nfds, int timeout` | Monitors multiple file descriptors for readability/writeability. |
| **8** | `SYS_LSEEK` | `int fd, off_t offset, int whence` | Moves read/write offset pointer of `fd`. |
| **9** | `SYS_MMAP` | `void *addr, size_t len, int prot, int flags, int fd, off_t offset` | Maps memory pages (used primarily for heap allocation). |
| **11** | `SYS_MUNMAP` | `void *addr, size_t len` | Unmaps memory pages allocated via `mmap`. |
| **12** | `SYS_BRK` | `void *addr` | Changes process data segment limit. |
| **13** | `SYS_RT_SIGACTION` | `int sig, const struct sigaction *act, struct sigaction *oact` | Gets/sets actions for process signals. |
| **14** | `SYS_RT_SIGPROCMASK` | `int how, const sigset_t *set, sigset_t *oset` | Manages process signal masks. |
| **16** | `SYS_IOCTL` | `int fd, unsigned long request, void *argp` | Performs driver/device-specific operations. |
| **22** | `SYS_PIPE` | `int pipefd[2]` | Allocates a pair of unified read/write pipes. |
| **24** | `SYS_SCHED_YIELD` | *none* | Yields the active CPU core to the next process. |
| **32** | `SYS_DUP` | `int oldfd` | Duplicates file descriptor `oldfd`. |
| **33** | `SYS_DUP2` | `int oldfd, int newfd` | Duplicates file descriptor `oldfd` onto `newfd`. |
| **35** | `SYS_NANOSLEEP` | `uint32_t milliseconds` | Suspends execution of current process. |
| **39** | `SYS_GETPID` | *none* | Returns the process identifier. |
| **41** | `SYS_SOCKET` | `int domain, int type, int protocol` | Creates a communication endpoint (TCP/UDP/Unix). |
| **42** | `SYS_CONNECT` | `int fd, const struct sockaddr *addr, socklen_t len` | Establishes socket connection. |
| **43** | `SYS_ACCEPT` | `int fd, struct sockaddr *addr, socklen_t *len` | Accepts incoming socket connection. |
| **44** | `SYS_SENDTO` | `int fd, const void *buf, size_t len, int flags, ...` | Transmits packet data over a socket. |
| **45** | `SYS_RECVFROM` | `int fd, void *buf, size_t len, int flags, ...` | Receives packet data from a socket. |
| **49** | `SYS_BIND` | `int fd, const struct sockaddr *addr, socklen_t len` | Binds a socket to a local port/address. |
| **50** | `SYS_LISTEN` | `int fd, int backlog` | Prepares a socket to accept connections. |
| **57** | `SYS_FORK` | *none* | Clones the current process. |
| **59** | `SYS_EXECVE` | `const char *pathname, char *const argv[], char *const envp[]` | Replaces active process memory image with an ELF. |
| **60** | `SYS_EXIT` | `int status` | Terminates process execution. |
| **61** | `SYS_WAIT4` | `pid_t pid, int *wstatus, int options, void *rusage` | Waits for a child process to terminate. |
| **62** | `SYS_KILL` | `pid_t pid, int sig` | Sends a signal to target PID. |
| **72** | `SYS_FCNTL` | `int fd, int cmd, int val` | Manages file descriptor flags (e.g., `O_NONBLOCK`). |
| **73** | `SYS_RT_SIGPENDING` | `sigset_t *set` | Returns signals currently pending delivery. |
| **79** | `SYS_GETCWD` | `char *buf, size_t size` | Gets current working directory path string. |
| **80** | `SYS_CHDIR` | `const char *path` | Changes current working directory. |
| **83** | `SYS_MKDIR` | `const char *path` | Creates a VFS directory. |
| **87** | `SYS_UNLINK` | `const char *path` | Deletes a file/node from VFS. |
| **158** | `SYS_ARCH_PRCTL` | `int code, unsigned long addr` | Sets architecture parameters (e.g. `ARCH_SET_FS` for thread-local storage). |
| **202** | `SYS_FUTEX` | `uint32_t *uaddr, int op, uint32_t val` | Fast userspace locking mechanism (`FUTEX_WAIT`, `FUTEX_WAKE`). |

---

## 2. Custom BoredOS System Calls (300+)

Specialized direct calls for disk partition control, virtual terminals, and quick VFS operations:

| Number | Symbol / Enum | Arguments | Description |
| :--- | :--- | :--- | :--- |
| **300** | `SYS_LIST_OFFSET` | `const char *path, FAT32_FileInfo *entries, int max, int offset` | Lists directory contents. |
| **301** | `SYS_SIZE` | `int fd` | Returns the total size of file descriptor `fd`. |
| **302** | `SYS_TELL` | `int fd` | Returns the current seek position of file descriptor `fd`. |
| **303** | `SYS_EXISTS` | `const char *path` | Checks if `path` exists in the VFS. |
| **304** | `SYS_FS_STATFS` | `const char *path, vfs_statfs_t *stat` | Reads filesystem capacity data. |
| **305** | `SYS_FS_MOUNT_COUNT` | *none* | Returns the count of mounted filesystems. |
| **306** | `SYS_FS_MOUNT_INFO` | `int index, mount_info_t *info` | Gets details for mount at `index`. |
| **317** | `SYS_SPAWN` | `const char *path, char *const argv[], char *const envp[], uint32_t flags` | Spawns a process. |
| **322** | `SYS_DISK_GET_COUNT` | *none* | Returns the count of recognized physical disks. |
| **323** | `SYS_DISK_GET_INFO` | `int index, disk_info_t *out` | Gets drive model, size, and partition tables. |
| **324** | `SYS_DISK_WRITE_GPT` | `const char *devname, partition_spec_t *parts, int count` | Writes GPT table to target block device. |
| **325** | `SYS_DISK_WRITE_MBR` | `const char *devname, partition_spec_t *parts, int count` | Writes MBR partition layout. |
| **326** | `SYS_DISK_MKFS_FAT32` | `const char *devname, const char *label` | Formats a block volume with a FAT32 filesystem. |
| **327** | `SYS_DISK_MOUNT` | `const char *devname, const char *mountpoint` | Mounts a device to a VFS target location. |
| **328** | `SYS_DISK_UMOUNT` | `const char *mountpoint` | Unmounts a filesystem from VFS. |
| **329** | `SYS_DISK_SYNC` | `const char *mountpoint` | Commits write cache blocks back to storage. |
| **330** | `SYS_DISK_RESCAN` | `const char *devname` | Reloads device partition mappings. |
| **349** | `SYS_REBOOT` | *none* | Triggers system warm reboot. |
| **350** | `SYS_SHUTDOWN` | *none* | Powers off the machine using ACPI. |

---