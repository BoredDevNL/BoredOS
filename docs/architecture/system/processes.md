# Process Management & Scheduling

Source: [`sys/process.c`](../../sys/process.c), [`sys/process.h`](../../sys/process.h)

BoredOS implements preemptive multitasking across SMP cores.

## 1. Process Structure (`process_t`)

Defined in [`sys/process.h`](../../sys/process.h), `process_t` tracks state for each task:

- **Identification**: `pid`, `parent_pid`, `tgid`, `pgid`, and `name`.
- **Memory**:
  - `vmm_space`: Pointer to the [virtual address space (`vmm_space_t`)](../memory/memory.md#virtual-memory-manager-vmm) containing the VMA tree and MMU context.
  - `pml4_phys`: Physical PML4 address for quick `CR3` loading.
  - `kernel_stack` & `user_stack_alloc`: Stack memory pointers.
  - `heap_start` & `heap_end`: Heap break bounds.
- **Context**:
  - `rsp`: Saved kernel stack pointer during context switch.
  - `fs_base`: TLS base address set via `SYS_ARCH_PRCTL` (`ARCH_SET_FS`).
  - `fpu_initialized` & `fxsave_region`: SSE/FPU state.
- **State & Scheduling**: `state` (`RUNNING`, `BLOCKED`, `ZOMBIE`), `ticks`, `sleep_until`, `cpu_affinity`, `is_idle`.
- **Resources**:
  - `fds`: File descriptor table tracking up to 64 open descriptors (`MAX_PROCESS_FDS = 64`).
  - `fd_kind`: Descriptor types (file, pipe read, pipe write, TTY, socket).
  - `tty_id`: Controlling TTY (0 to 9).
  - `cwd`: Current working directory path.
- **Signals & Synchronization**: `signal_mask`, `signal_pending`, `wait_exit_queue`.

---

## 2. Scheduler

- **Round-Robin Scheduling**: The scheduler runs on each CPU core driven by timer interrupts (vector `0x20`) and inter-core IPIs (vector `0x41`).
- **CPU Affinity**: Processes specify CPU affinity masks (`cpu_affinity`). Tasks can be pinned to specific cores or scheduled across all AP cores.
- **Switching**:
  1. Saves current `rsp` in the active process struct.
  2. Traverses the runnable queue for the next matching process.
  3. Updates TSS `RSP0` for interrupt stack handling.
  4. Switches address space by loading `pml4_phys` into `CR3`.
  5. Restores registers and resumes execution via `iretq` or `sysret`.

---

## 3. Fork, Clone, and Exec

- **`SYS_FORK` / `SYS_CLONE`**:
  - Allocates a child `process_t`.
  - Duplicates address space using `vmm_clone_space()`, setting up copy-on-write page table mappings.
  - Duplicates file descriptors and inherits environment state.
- **`SYS_EXECVE`**:
  - Frees or resets existing address space VMAs.
  - Reads the new ELF binary, maps program segments into VMAs, demand-pages code/data.
  - Allocates a fresh user stack and pushes `argv` / `envp` vectors.
  - Resets register state and jumps to the ELF entry point in Ring 3.
