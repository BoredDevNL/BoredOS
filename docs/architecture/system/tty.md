# BoredOS TTY, Serial Devices & Headless Boot Architecture

BoredOS unifies graphical virtual terminals, hardware ISA UARTs, and dynamic pseudo-terminals (PTYs) into a single stream-oriented character device layer. 

This document defines the ID routing scheme, hardware interrupt dispatching, headless boot degradation, serial line discipline, and VFS device registration.

## 1. Subsystem Architecture & ID Routing

TTY routing relies on a hard partition at `PTY_ID_BASE` (1024): IDs `0–1023` map directly to static index offsets in hardware driver tables, while IDs `1024+` route to dynamic PTY descriptors.

| TTY ID Range | Class | Backing Module | Purpose & Implementation |
| :--- | :--- | :--- | :--- |
| `0` – `9` | Graphical VT (`/dev/tty1`–`/dev/tty10`) | [`sys/tty.c`](../../../sys/tty.c) | Virtual consoles backed by 2D text cell grids. Blitted to Limine framebuffer via `tty_blit_active()`. |
| `10` – `13` | Serial TTY (`/dev/ttyS0`–`/dev/ttyS3`) | [`dev/serial.c`](../../../dev/serial.c) | Legacy ISA UART ports (COM1–COM4). Writes bypass grid storage straight to UART TX registers. |
| `1024` + | Pseudo-Terminals (`/dev/pts/N`) | [`sys/pty.c`](../../../sys/pty.c) | Dynamic PTY pairs for shells, windowed terminal emulators, and SSH daemons. |

### Graphical Virtual Consoles (`tty1`–`tty10`)
* **Storage:** Allocates an array of `cols * rows` [`tty_cell_t`](../../../sys/tty.h) structures (codepoint, fg color, bg color).
* **Rendering:** `blit_enabled = true`. When active (`g_active_tty`), `kmain()` invokes `tty_blit_active()` every 16ms to draw dirty cells directly to the Limine framebuffer.
* **State:** Tracks cursor position and ANSI escape sequences in software.

### Serial TTY Devices (`ttyS0`–`ttyS3`)
* **Storage:** No grid allocation (`is_serial = true`, `grid = NULL`, `blit_enabled = false`).
* **I/O:** Bypasses font rendering; raw bytes are piped directly out through the mapped port's transmit hold register (`COM1_PORT`..`COM4_PORT`).

## 2. Pseudo-Terminals (PTYs)

Dynamic terminal sessions allocate master/slave pairs out of a fixed table managed in [`sys/pty.c`](../../../sys/pty.c).

```c
#define PTY_ID_BASE 1024
#define PTY_MAX_COUNT 4096

typedef struct {
    int id;
    bool used;
    pty_queue_t master_to_slave;
    pty_queue_t slave_to_master;
    int fg_pid;
    struct winsize ws;
    spinlock_t lock;
} pty_pair_t;
```

### Allocation Model
* **Lookup:** `pty_get(id)` performs an O(1) array access indexed at `id - PTY_ID_BASE`.
* **Allocation:** `pty_create()` scans `pty_pairs[]` for the first unused slot up to `PTY_MAX_COUNT`.
* **Teardown:** When a master FD closes, `used` is set to `false`, freeing the index for subsequent `posix_openpt()` calls.

---

## 3. Serial Driver & Interrupt Pipeline

The driver in [`dev/serial.c`](../../../dev/serial.c) probes ISA legacy I/O ports at boot, registers detected devices, and sets up ring buffers for asynchronous interrupt-driven reception.

| Port Constant | I/O Base | IRQ Line | Default Device Node |
| :--- | :--- | :--- | :--- |
| `COM1_PORT` | `0x3F8` | IRQ 4 | `/dev/ttyS0` (TTY 10) |
| `COM2_PORT` | `0x2F8` | IRQ 3 | `/dev/ttyS1` (TTY 11) |
| `COM3_PORT` | `0x3E8` | IRQ 4 | `/dev/ttyS2` (TTY 12) |
| `COM4_PORT` | `0x2E8` | IRQ 3 | `/dev/ttyS3` (TTY 13) |

### Probing & Registration
During early boot, `serial_init()` performs a scratch-register probe (`outb(port + 7, 0xAE)` $\to$ `inb(port + 7) == 0xAE`). Successfully identified ports register into the subsystem:

```c
typedef struct serial_device {
    int id;
    serial_type_t type;   // SERIAL_TYPE_ISA, SERIAL_TYPE_PCI_UART, SERIAL_TYPE_USB_ACM
    uint16_t io_port;     // Legacy I/O port base address
    uintptr_t mmio_base;  // Memory Mapped I/O base address
    uint8_t irq;
    bool is_present;
} serial_device_t;
```

### Interrupt Flow
1. **PIC Masking:** In [`sys/idt.c`](../../../sys/idt.c), the 8259 PIC unmasks IRQ 3 and IRQ 4 (`outb(0x21, 0xE0)`).
2. **IDT Dispatch:** Interrupt vectors 35 (`isr3_wrapper`) and 36 (`isr4_wrapper`) invoke `serial_com2_handler()` and `serial_com1_handler()`.
3. **FIFO Drain:** [`serial_irq_handler()`](../../../dev/serial.c) pulls incoming bytes out of the UART FIFO into a 1024-byte ring buffer (`serial_ring_t`) and flushes them to the TTY layer via `tty_push_serial_char(GRAPHICAL_TTY_COUNT + dev_index, c)`.

## 4. Headless Mode & Log Multiplexing

If display hardware is absent or explicitly disabled via kernel boot flags, the system falls back to headless serial execution.

### Mode Determination
Inside `init_graphics()` ([`core/main.c`](../../../core/main.c)), `g_headless_mode` is set to `true` if:
* The kernel command line contains `headless=1` or `--headless`.
* Limine returns zero display buffers (`framebuffer_count < 1`).

### Runtime Behavior Differences

* **Graphical Mode (`g_headless_mode == false`):**
  * `init_tty()` spawns 10 shells (`/bin/bsh.elf 1` .. `/bin/bsh.elf 10`) on `/dev/tty1`–`/dev/tty10`.
  * `COM1` is dedicated to kernel logging (`serial_write`, `log_ok`).
  * `kmain()` executes `tty_blit_active()` every 16ms.

* **Headless Mode (`g_headless_mode == true`):**
  * Framebuffer setup and graphical VT shells (`tty1`–`tty10`) are omitted entirely.
  * A single shell (`/bin/bsh.elf 10`) spawns attached directly to `/dev/ttyS0`.
  * The `tty_blit_active()` call in `kmain()` is bypassed to avoid wasted render cycles.

### Log Collision Prevention
To prevent kernel debug logs from corrupting interactive shell prompts over the same physical wire on headless boots, `serial_set_debug_port()` re-routes logging based on available ports:

```c
if (g_headless_mode) {
    if (serial_is_com2_present()) {
        serial_set_debug_port(COM2_PORT); // Shell on COM1, debug logs redirected to COM2
        serial_set_log_silenced(false);
    } else {
        serial_set_debug_port(COM1_PORT); // Only COM1 exists; suppress kernel logs to keep shell usable
        serial_set_log_silenced(true);
    }
}
```

## 5. Serial Line Discipline & In-Band Signals

Because serial streams transmit raw ASCII bytes instead of hardware keyboard scancodes, [`tty_push_serial_char()`](../../../sys/tty.c) acts as a software line discipline to normalize stream anomalies and handle in-band POSIX control characters.

* **CR / CRLF Normalization:** Terminal emulators often transmit `\r` (ASCII 13) or `\r\n` on Enter. `\r` is pushed as `\n` to the input queue, and `last_char_was_cr` is flagged so any immediate subsequent `\n` is discarded to prevent double-spaced newlines.
* **Backspace Normalization:** Both `\b` (ASCII 8) and `DEL` (ASCII 127 / `0x7F`) are mapped to `\b`.
* **In-Band `SIGINT` (Ctrl+C):** An incoming `0x03` byte triggers signal delivery directly against the active foreground PID:

```c
if (ch == CTRL_C_CHAR) {
    int fg = t->fg_pid;
    process_t *target = (fg > 0) ? process_get_by_pid((uint32_t)fg) : process_find_child_on_tty(id);

    if (target && target->pid > 1) {
        if (t->serial_dev) {
            serial_device_write_str(t->serial_dev, "^C\r\n"); // Visual echo back to terminal
        }
        target->signal_pending |= SIGINT;
        if (target->state == PROC_STATE_BLOCKED) {
            target->state = PROC_STATE_RUNNING;
            target->sleep_until = 0;
        }
        t->fg_pid = -1;
        return;
    }
}
```

## 6. VFS Integration & Device Nodes

The Virtual File System ([`fs/vfs.c`](../../../fs/vfs.c)) exposes TTY and PTY interfaces under `/dev/` and guards graphical nodes during headless boots.

### Device Node Mapping

| Path | Mode Filter | Internal ID Mapping | Description |
| :--- | :--- | :--- | :--- |
| `/dev/tty1` – `/dev/tty10` | Graphical Only | `0` – `9` | Virtual graphical console devices. |
| `/dev/ttyS0` – `/dev/ttyS3` | All Modes | `10` – `13` | Hardware serial UART devices (COM1–COM4). |
| `/dev/pts/N` | All Modes | `1024 + N` | Dynamically allocated PTY slave nodes. |
| `/dev/console` | All Modes | Graphical: `0` (`tty1`)<br>Headless: `10` (`ttyS0`) | Primary system boot console alias. |
| `/dev/tty` | All Modes | Calling Process's Controlling TTY | Current process controlling terminal alias. |

### Headless Runtime Protection
When `g_headless_mode == true`:
* `vfs_open()` immediately rejects requests to `/dev/tty1`–`/dev/tty10` and `/dev/fb0` with `NULL`.
* `vfs_list_directory("/dev")` filters out `tty1`–`tty10` and `fb0` from directory listings.

### Supported IOCTL Commands

| Request Code | Name | Description |
| :--- | :--- | :--- |
| `0x541F` | `TIOCISSERIAL` | Returns `1` if device is backed by UART, `0` for graphical VTs. |
| `0x5606` | `VT_ACTIVATE` | Sets `g_active_tty` to switch the active visible virtual console. |
| `0x4B3A` | `KDSETMODE` | Toggles between `KD_TEXT` (active blit loop) and `KD_GRAPHICS` (disables blit for direct framebuffer draws). |
| `0x5413` / `0x5414` | `TIOCGWINSZ` / `TIOCSWINSZ` | Reads/writes `struct winsize` (row/column counts). |
| `0x5430` | `TIOCGPTN` | Extracts the PTY slave index number from a master FD. |
