# libc Reference

This page documents the current BoredOS userland libc surface from `contrib/libc/include/`.

BoredOS libc is a compact implementation focused on the APIs used by in-tree apps. It is not a full glibc replacement.

## Header Overview

| Header | Focus |
|---|---|
| `stdlib.h` | allocation, conversion, process helpers |
| `string.h` | memory/string primitives |
| `stdio.h` | `FILE*` and formatted I/O |
| `unistd.h` | POSIX-like fd/process calls |
| `fcntl.h` | open/fcntl flags, `dup`, `pipe` |
| `poll.h` | event-driven I/O multiplexing |
| `signal.h` | signal handlers and masks |
| `sys/stat.h` | `stat`/`fstat` and file mode bits |
| `sys/types.h` | core typedefs (`pid_t`, `ssize_t`, ...) |
| `sys/wait.h` | `waitpid` and wait macros |
| `sys/ioctl.h` | device-specific I/O control operations |
| `sys/kd.h` | console display mode definitions |
| `errno.h` | errno values |
| `time.h` | time/date utilities |
| `math.h` | floating-point math helpers |

## stdlib.h

Implemented core functions:
- Memory: `malloc`, `free`, `calloc`, `realloc`
- Memory aliases: `memset`, `memcpy`
- Conversions: `atoi`, `itoa`, `strtod`, `abs`
- Output: `puts`, `printf`
- Process/environment: `exit`, `_exit`, `sleep`, `chdir`, `getcwd`, `access`, `system`, `getenv`, `abort`

Notes:
- `sleep` is millisecond-based and maps to kernel sleep command.
- `system` is a stub-style helper in this libc, not a full shell launcher equivalent.

## string.h

Implemented C string/memory set includes:
- Memory: `memmove`, `memcmp`, `memcpy`, `memset`, `memchr`
- Search: `strchr`, `strrchr`, `strpbrk`, `strstr`
- Span: `strspn`, `strcspn`
- Compare: `strcmp`, `strncmp`, `strcasecmp`, `strncasecmp`, `strcoll`
- Build/copy: `strlen`, `strcpy`, `strcat`, `strdup`
- Errors: `strerror`

## stdio.h

Provided API includes:
- Stream open/close: `fopen`, `freopen`, `fclose`
- Read/write: `fread`, `fwrite`, `fgets`, `fputs`, `getc`, `fputc`, `putchar`
- Positioning: `fseek`, `ftell`, `filelength`
- Formatting: `fprintf`, `vfprintf`, `snprintf`, `vsnprintf`, `sprintf`, `sscanf`
- Stream state: `feof`, `ferror`, `clearerr`, `fflush`, `ungetc`
- Temp/filesystem helpers: `remove`, `rename`, `tmpfile`, `tmpnam`

## unistd.h

Provided POSIX-like interfaces:
- FD I/O: `read`, `write`, `close`, `lseek`, `isatty`
- Filesystem: `unlink`
- Exec family: `execv`, `execve`, `execvp`, `execl`, `execlp`, `execle`
- Process wait: `waitpid`

Also defines:
- `SEEK_SET`, `SEEK_CUR`, `SEEK_END`
- `F_OK`, `X_OK`, `W_OK`, `R_OK`

## fcntl.h

Flags and fd control:
- Open flags: `O_RDONLY`, `O_WRONLY`, `O_RDWR`, `O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`, `O_NONBLOCK`, `O_ACCMODE`
- fcntl ops: `F_GETFL`, `F_SETFL`
- FD flag: `FD_CLOEXEC` (declared)

Functions:
- `open`
- `fcntl`
- `dup`
- `dup2`
- `pipe`



## signal.h

Current signal surface:
- Basic handler API: `signal`, `raise`, `kill`
- POSIX-style API: `sigaction`, `sigprocmask`, `sigpending`
- Types: `sighandler_t`, `sigset_t`, `struct sigaction`
- Constants: `SIGINT`, `SIGTERM`, `SIGKILL`, `SIG_DFL`, `SIG_IGN`, `SIG_ERR`
- Mask ops: `SIG_BLOCK`, `SIG_UNBLOCK`, `SIG_SETMASK`
- Action flags: `SA_RESTART`, `SA_NODEFER`, `SA_RESETHAND`

## ctype.h

Character classification and case conversion:
- `isdigit`, `isalpha`, `isalnum`, `isspace`
- `isupper`, `islower`, `isxdigit`
- `iscntrl`, `ispunct`, `isprint`, `isgraph`
- `tolower`, `toupper`

## locale.h

Locale stubs and conventions:
- `struct lconv`
- `setlocale`
- `localeconv`
- `LC_ALL`

## limits.h

Integer and floating-point limit macros:
- `CHAR_BIT`, `INT_MIN`, `INT_MAX`, `UINT_MAX`
- `LONG_MIN`, `LONG_MAX`, `ULONG_MAX`
- `LLONG_MIN`, `LLONG_MAX`, `ULLONG_MAX`
- `DBL_MAX`

## setjmp.h

Non-local jump support:
- `jmp_buf`
- `setjmp`
- `longjmp`

## time.h

Time/date APIs and types:
- Types: `time_t`, `clock_t`, `struct tm`
- Constants: `CLOCKS_PER_SEC`
- Functions: `time`, `clock`, `localtime`, `gmtime`, `strftime`, `mktime`

## sys/ioctl.h and sys/kd.h

`sys/ioctl.h` provides device-specific input/output control operations:
- Framebuffer info queries (`FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`)
- TTY console window size queries (`TIOCGWINSZ`)
- Console keyboard display mode settings (`KDSETMODE`)

`sys/kd.h` provides console display constants:
- `KDSETMODE` ioctl command (`0x4B3A`)
- `KD_TEXT` standard console mode (`0x00`) to restore text blitting
- `KD_GRAPHICS` graphics mode (`0x01`) to stop TTY text blitting for raw `/dev/fb0` access

## sys/stat.h and sys/types.h

`sys/stat.h` provides:
- `struct stat`
- `stat`, `fstat`, `mkdir`
- mode/type macros (`S_IFREG`, `S_IFDIR`, `S_ISREG`, `S_ISDIR`, permission bits)

Note:
- `access` is declared in `stdlib.h` in this libc.

`sys/types.h` provides:
- `ssize_t`, `off_t`, `mode_t`, `pid_t`, `uid_t`, `gid_t`

## sys/wait.h

- `waitpid`
- `WNOHANG`
- status macros: `WEXITSTATUS`, `WIFEXITED`, `WTERMSIG`, `WIFSIGNALED`

## errno.h

Defined errno values include:
- Generic/input: `EINVAL`, `EDOM`, `ERANGE`, `E2BIG`
- File/path: `ENOENT`, `EEXIST`, `EISDIR`, `ENOTDIR`, `EBADF`
- Runtime/state: `ENOMEM`, `EACCES`, `EIO`, `EAGAIN`, `EINTR`, `ECHILD`, `EBUSY`, `EPIPE`, `ESPIPE`, `ENOSYS`, `ENOTSUP`

## Relationship to raw syscalls

- libc high-level I/O and process APIs are backed by wrappers in `contrib/libc/src/syscall.c`.
- Full syscall command IDs and multiplexer details are documented in `docs/appdev/syscalls.md`.

## Practical Guidance

- Prefer libc APIs (`open`, `read`, `write`, `waitpid`, `sigaction`) for portability inside BoredOS userland.
- Use raw wrapper calls from `syscall.h` only for capabilities that do not yet have higher-level libc wrappers.
- Avoid numeric `sys_system(...)` command literals in app code; use `SYSTEM_CMD_*` macros.
