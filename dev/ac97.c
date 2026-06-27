// Copyright (c) 2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.

#include "ac97.h"
#include "pci.h"
#include "io.h"
#include "memory_manager.h"
#include "platform.h"
#include "spinlock.h"
#include "kutils.h"
#include "process.h"
#include "idt.h"
#include "errno.h"

// OSS-compatible DSP ioctl numbers, defined locally to avoid userspace headers.
#define SNDCTL_DSP_RESET      0x5001
#define SNDCTL_DSP_SYNC       0x5002
#define SNDCTL_DSP_SPEED      0x5003
#define SNDCTL_DSP_STEREO     0x5004
#define SNDCTL_DSP_GETFMTS    0x5005
#define SNDCTL_DSP_SETFMT     0x5006
#define SNDCTL_DSP_CHANNELS   0x5007

#define AFMT_S16_LE           0x0010
#define AFMT_U8               0x0008

// OSS-compatible mixer ioctl numbers.
#define SOUND_MIXER_READ_VOLUME  0x6001
#define SOUND_MIXER_WRITE_VOLUME 0x6002
#define SOUND_MIXER_READ_PCM     0x6003
#define SOUND_MIXER_WRITE_PCM    0x6004
#define SOUND_MIXER_READ_MIC     0x6005
#define SOUND_MIXER_WRITE_MIC    0x6006

extern void serial_write(const char *str);
extern void serial_write_hex(uint64_t val);
extern void serial_write_num(uint64_t num);

static pci_device_t ac97_dev;
static bool ac97_found = false;
static uint16_t nam_base = 0;  // Native Audio Mixer I/O base (BAR0)
static uint16_t nabm_base = 0; // Native Audio Bus Master I/O base (BAR1)

// Buffer Descriptor List and its physical address for DMA.
static ac97_bd_t *bdl = NULL;
static uint32_t bdl_phys = 0;

// 32 DMA buffers (one per BDL slot), each 16 KB.
static uint8_t *dma_buffers[32];
static uint32_t dma_buffers_phys[32];

// Index of the next BDL slot to fill (wraps at 32).
static int write_idx = 0;
static spinlock_t ac97_lock = SPINLOCK_INIT;
static bool vra_supported = false;

// Software volume (0-100 per channel). QEMU's AC97 emulation stores the NAM
// volume registers correctly but does not apply them to the DMA audio stream,
// so we scale samples here in the mixer thread instead.
static int sw_vol_l = 100, sw_vol_r = 100; // Master volume
static int sw_pcm_l = 100, sw_pcm_r = 100; // PCM/DAC volume

static int32_t mix_buf[4096 * 2];
static int16_t client_temp_buf[8192 * 2];

typedef struct {
    int count;
    spinlock_t lock;
    wait_queue_head_t waitq;
} semaphore_t;

static semaphore_t mixer_sem;

static inline void sched_yield(void) {
    asm volatile("int $128" : : "a"(5), "D"(43) : "rcx", "r11", "memory");
}

static void sem_init(semaphore_t *sem, int initial_count) {
    sem->count = initial_count;
    sem->lock = SPINLOCK_INIT;
    wait_queue_init(&sem->waitq);
}

static void sem_wait(semaphore_t *sem) {
    process_t *proc = process_get_current();
    wait_queue_entry_t entry;
    entry.proc = proc;
    entry.next = NULL;
    wait_queue_add(&sem->waitq, &entry);

    while (1) {
        uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
        if (sem->count > 0) {
            sem->count--;
            spinlock_release_irqrestore(&sem->lock, flags);
            wait_queue_remove(&sem->waitq, &entry);
            return;
        }

        proc->state = PROC_STATE_BLOCKED;
        spinlock_release_irqrestore(&sem->lock, flags);

        sched_yield();
    }
}

static void sem_signal(semaphore_t *sem) {
    uint64_t flags = spinlock_acquire_irqsave(&sem->lock);
    sem->count++;
    spinlock_release_irqrestore(&sem->lock, flags);
    wait_queue_wake_all(&sem->waitq);
}

#define MAX_AUDIO_CLIENTS 8
#define CLIENT_BUFFER_SIZE (128 * 1024) // Per-client ring buffer: 128 KB

// State for a single software audio client. Each client has its own ring
// buffer and sample-rate tracking; the mixer thread blends all active clients
// into the hardware DMA buffers.
typedef struct {
    bool active;
    int speed;    // Source sample rate in Hz (e.g. 44100, 48000)
    int channels; // Informational only; output is always stereo
    int format;   // Informational only; output is always AFMT_S16_LE

    uint8_t *buffer;    // Ring buffer (CLIENT_BUFFER_SIZE bytes)
    uint32_t write_pos; // Next byte position for incoming audio data
    uint32_t read_pos;  // Next byte position for the mixer to consume
    uint32_t count;     // Number of bytes currently available to mix
    uint64_t phase;     // 32.32 fixed-point phase accumulator for sample-rate conversion

    spinlock_t lock;
    wait_queue_head_t write_waitq;
    wait_queue_head_t sync_waitq;
} audio_client_t;

static audio_client_t audio_clients[MAX_AUDIO_CLIENTS];
// Global mixer spinlock.
// Lock Hierarchy: Always acquire `mixer_lock` before any `client->lock` to prevent deadlocks.
static spinlock_t mixer_lock = SPINLOCK_INIT;

// Read one stereo frame (4 bytes) at frame_offset frames past the client's
// current read position, wrapping around the ring buffer as needed.
static inline void get_client_frame(audio_client_t *client, uint32_t frame_offset, int16_t *out_l, int16_t *out_r) {
    uint32_t byte_offset = (client->read_pos + frame_offset * 4) % CLIENT_BUFFER_SIZE;
    int16_t *samples = (int16_t *)(client->buffer + byte_offset);
    *out_l = samples[0];
    *out_r = samples[1];
}

bool ac97_present(void) {
    return ac97_found;
}

// Returns true if the PCM Output DMA channel is running and has not halted.
// NABM+0x1B: PCM Out Control; bit 0 = run/pause.
// NABM+0x16: PCM Out Status;  bit 0 = DCH (DMA controller halted).
static bool is_dma_active(void) {
    bool run_bit = (inb(nabm_base + 0x1B) & 0x01) != 0;
    bool halted_status = (inw(nabm_base + 0x16) & 0x01) != 0;
    return run_bit && !halted_status;
}

// Kernel thread that continuously mixes active audio clients into DMA buffers
// and feeds them to the AC97 PCM Output channel.
static void ac97_mixer_thread(void) {
    while (true) {
        sem_wait(&mixer_sem);

        // Keep the DMA queue filled with up to 3 pending buffers (~255 ms of audio).
        while (true) {
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            bool active = is_dma_active();
            if (active) {
                // CIV (NABM+0x14) = current index being played; LVI (NABM+0x15) = last valid index.
                uint8_t cpe = inb(nabm_base + 0x14); // Current Index Value (currently playing)
                uint8_t lve = inb(nabm_base + 0x15); // Last Valid Index (last queued buffer)
                // The hardware wraps around a 32-entry buffer descriptor list.
                // Distance from CPE to LVE represents active buffers queued.
                // Since CIV == LVI means 1 entry is actively being processed before halt,
                // the queue depth is (LVE - CPE + 32) % 32 + 1.
                int queued = (lve - cpe + 32) % 32 + 1;
                if (queued >= 3) {
                    spinlock_release_irqrestore(&ac97_lock, flags);
                    break;
                }
            } else {
                // DMA stopped; reset the channel before submitting new buffers.
                outb(nabm_base + 0x1B, 0x02); // RR: reset channel registers
                int timeout = 100000;
                while ((inb(nabm_base + 0x1B) & 0x02) && --timeout > 0);
                write_idx = 0;
            }
            spinlock_release_irqrestore(&ac97_lock, flags);

            // Do not queue silence: stop if no client has data remaining.
            bool any_data = false;
            flags = spinlock_acquire_irqsave(&mixer_lock);
            for (int i = 0; i < MAX_AUDIO_CLIENTS; i++) {
                if (audio_clients[i].active && audio_clients[i].count > 0) {
                    any_data = true;
                    break;
                }
            }
            spinlock_release_irqrestore(&mixer_lock, flags);

            if (!any_data) {
                break;
            }

            memset(mix_buf, 0, sizeof(mix_buf));

            flags = spinlock_acquire_irqsave(&mixer_lock);
            for (int c = 0; c < MAX_AUDIO_CLIENTS; c++) {
                audio_client_t *client = &audio_clients[c];
                uint64_t c_flags = spinlock_acquire_irqsave(&client->lock);
                if (client->active && client->count > 0) {
                    uint64_t phase = client->phase;
                    int speed = client->speed;
                    if (speed <= 0) speed = 48000;
                    uint64_t phase_step = ((uint64_t)speed << 32) / 48000;

                    // Calculate how many source frames we need for linear interpolation:
                    // ((phase + 4096 * phase_step) >> 32) + 2 (extra 1 frame for idx + 1 safety)
                    uint32_t consumed_frames = ((phase + (uint64_t)4096 * phase_step) >> 32) + 2;
                    if (consumed_frames > 8192) {
                        consumed_frames = 8192;
                    }
                    uint32_t avail_frames = client->count / 4;
                    uint32_t frames_to_copy = consumed_frames;
                    if (frames_to_copy > avail_frames) {
                        frames_to_copy = avail_frames;
                    }

                    uint32_t bytes_to_copy = frames_to_copy * 4;
                    uint32_t space_to_end = CLIENT_BUFFER_SIZE - client->read_pos;
                    if (bytes_to_copy <= space_to_end) {
                        memcpy(client_temp_buf, client->buffer + client->read_pos, bytes_to_copy);
                    } else {
                        memcpy(client_temp_buf, client->buffer + client->read_pos, space_to_end);
                        memcpy(((uint8_t*)client_temp_buf) + space_to_end, client->buffer, bytes_to_copy - space_to_end);
                    }

                    if (frames_to_copy < consumed_frames) {
                        memset(((uint8_t*)client_temp_buf) + bytes_to_copy, 0, (consumed_frames - frames_to_copy) * 4);
                    }

                    uint64_t end_phase = phase + (uint64_t)4096 * phase_step;
                    uint32_t consumed_actual_frames = (uint32_t)(end_phase >> 32);
                    uint32_t consumed_actual_bytes = consumed_actual_frames * 4;
                    bool clamped = false;
                    if (consumed_actual_bytes > client->count) {
                        consumed_actual_bytes = client->count;
                        clamped = true;
                    }

                    client->read_pos = (client->read_pos + consumed_actual_bytes) % CLIENT_BUFFER_SIZE;
                    client->count   -= consumed_actual_bytes;

                    bool wake_sync = false;
                    if (client->count == 0 || clamped) {
                        wake_sync = true;
                        client->phase = 0;
                    } else {
                        client->phase = end_phase - ((uint64_t)consumed_actual_frames << 32);
                    }

                    // Release client->lock before running the mix loop and doing wakeups to reduce lock hold times
                    spinlock_release_irqrestore(&client->lock, c_flags);

                    if (wake_sync) {
                        wait_queue_wake_all(&client->sync_waitq);
                    }
                    wait_queue_wake_all(&client->write_waitq);

                    // Perform linear interpolation and mixing from client_temp_buf lock-free
                    for (int i = 0; i < 4096; i++) {
                        uint64_t src_phase = phase + i * phase_step;
                        uint32_t idx  = src_phase >> 32;
                        uint32_t frac = (src_phase & 0xFFFFFFFF) >> 16;

                        int16_t l = 0, r = 0;
                        if (idx < frames_to_copy) {
                            int16_t l1 = client_temp_buf[idx * 2];
                            int16_t r1 = client_temp_buf[idx * 2 + 1];
                            if (idx + 1 < frames_to_copy) {
                                int16_t l2 = client_temp_buf[(idx + 1) * 2];
                                int16_t r2 = client_temp_buf[(idx + 1) * 2 + 1];
                                l = l1 + ((l2 - l1) * (int32_t)frac >> 16);
                                r = r1 + ((r2 - r1) * (int32_t)frac >> 16);
                            } else {
                                l = l1;
                                r = r1;
                            }
                        }

                        mix_buf[i * 2]     += l;
                        mix_buf[i * 2 + 1] += r;
                    }
                } else {
                    spinlock_release_irqrestore(&client->lock, c_flags);
                }
            }
            spinlock_release_irqrestore(&mixer_lock, flags);

            int16_t *dst = (int16_t *)dma_buffers[write_idx];
            int32_t vol_l_fp = (sw_vol_l * sw_pcm_l * 65536) / 10000;
            int32_t vol_r_fp = (sw_vol_r * sw_pcm_r * 65536) / 10000;
            for (int i = 0; i < 4096 * 2; i += 2) {
                int32_t l = (mix_buf[i]     * vol_l_fp) >> 16;
                int32_t r = (mix_buf[i + 1] * vol_r_fp) >> 16;
                if      (l >  32767) l =  32767;
                else if (l < -32768) l = -32768;
                if      (r >  32767) r =  32767;
                else if (r < -32768) r = -32768;
                dst[i]     = (int16_t)l;
                dst[i + 1] = (int16_t)r;
            }

            // Submit the buffer to the hardware.
            flags = spinlock_acquire_irqsave(&ac97_lock);
            bdl[write_idx].length = 8192; // 4096 stereo frames × 2 samples each
            bdl[write_idx].flags  = 0x8000; // IOC bit — fires interrupt when this buffer completes.

            if (!active) {
                // DMA was stopped: point the hardware at the BDL and start it.
                outl(nabm_base + 0x10, bdl_phys); // PCM Out BDL Base Address
                outb(nabm_base + 0x15, 0);         // LVI = 0
                outb(nabm_base + 0x1B, 0x1D);      // RPBM | LVBIE | IOCE | FEIE
            } else {
                // DMA is already running: advance the Last Valid Index.
                outb(nabm_base + 0x15, write_idx);
            }
            write_idx = (write_idx + 1) % 32;
            spinlock_release_irqrestore(&ac97_lock, flags);
        }
    }
}

uint64_t ac97_handler(registers_t *regs) {
    if (!ac97_found) return (uint64_t)regs;

    uint16_t sr = inw(nabm_base + 0x16);
    if (sr & 0x1C) {
        // Clear status bits (including interrupt flags and error bits)
        outw(nabm_base + 0x16, sr & 0x1C);

        // Log diagnostics on DMA FIFO error flag (FIFOE = bit 4)
        if (sr & 0x10) {
            serial_write("[AC97] DMA FIFO Error!\n");
        }

        sem_signal(&mixer_sem);

        outb(0x20, 0x20);
        outb(0xA0, 0x20);
    }

    return (uint64_t)regs;
}

void ac97_init(void) {
    serial_write("[AC97] Scanning PCI for AC97 audio controller...\n");

    // AC97 audio is PCI class 0x04 (Multimedia), subclass 0x01 (Audio).
    if (!pci_find_device_by_class(0x04, 0x01, &ac97_dev)) {
        serial_write("[AC97] No AC97 audio controller found\n");
        return;
    }

    serial_write("[AC97] Found AC97 audio controller (");
    serial_write("vendor=0x");
    serial_write_hex(ac97_dev.vendor_id);
    serial_write(", device=0x");
    serial_write_hex(ac97_dev.device_id);
    serial_write(")\n");

    // Enable I/O space access (bit 0) and Bus Mastering (bit 2) in the PCI Command register.
    uint32_t cmd = pci_read_config(ac97_dev.bus, ac97_dev.device, ac97_dev.function, 0x04);
    cmd |= (1 << 0) | (1 << 2);
    pci_write_config(ac97_dev.bus, ac97_dev.device, ac97_dev.function, 0x04, cmd);

    // BAR0 = Native Audio Mixer (codec registers), BAR1 = Native Audio Bus Master (DMA control).
    nam_base  = (uint16_t)pci_get_bar(&ac97_dev, 0);
    nabm_base = (uint16_t)pci_get_bar(&ac97_dev, 1);

    if (nam_base == 0 || nabm_base == 0) {
        serial_write("[AC97] Invalid BAR addresses\n");
        return;
    }

    serial_write("[AC97] NAM Base: 0x");
    serial_write_hex(nam_base);
    serial_write(", NABM Base: 0x");
    serial_write_hex(nabm_base);
    serial_write("\n");

    // NABM+0x2C: Global Control register. Set bit 1 (GIE) to release cold reset.
    outl(nabm_base + 0x2C, 0x00000002);
    k_delay(500000);
    uint32_t gc = inl(nabm_base + 0x2C);
    if (!(gc & 0x02)) {
        serial_write("[AC97] Warning: Cold reset bit not set, attempting warm reset...\n");
        outl(nabm_base + 0x2C, inl(nabm_base + 0x2C) | 0x04); // Bit 2 = warm reset
        k_delay(500000);
    }

    // Writing 0x0000 to NAM+0x00 resets all codec mixer registers to defaults.
    outw(nam_base + 0x00, 0x0000);
    k_delay(500000);

    // NAM+0x28: Extended Audio ID. Bit 0 indicates Variable Rate Audio (VRA) support.
    uint16_t ext_cap = inw(nam_base + 0x28);
    if (ext_cap & 0x01) {
        vra_supported = true;
        // Enable VRA via NAM+0x2A (Extended Audio Status/Control).
        outw(nam_base + 0x2A, inw(nam_base + 0x2A) | 0x01);
        // Set the front DAC rate to 48000 Hz (NAM+0x2C: PCM Front DAC Rate).
        outw(nam_base + 0x2C, 48000);
        k_delay(250000);
    }

    // NAM+0x02: Master Volume (0x0000 = 0 dB, unmuted).
    // NAM+0x04: Headphone/AUX Volume.
    // NAM+0x18: PCM Out Volume (hardware DAC gain).
    outw(nam_base + 0x02, 0x0000);
    outw(nam_base + 0x04, 0x0000);
    outw(nam_base + 0x18, 0x0808);

    // Allocate the Buffer Descriptor List (32 entries × 8 bytes, page-aligned for DMA).
    bdl = (ac97_bd_t*)kmalloc_aligned(32 * sizeof(ac97_bd_t), 4096);
    if (!bdl) {
        serial_write("[AC97] Failed to allocate BDL\n");
        return;
    }
    bdl_phys = (uint32_t)v2p((uint64_t)bdl);

    for (int i = 0; i < 32; i++) {
        dma_buffers[i] = (uint8_t*)kmalloc_aligned(16384, 4096);
        if (!dma_buffers[i]) {
            serial_write("[AC97] Failed to allocate DMA buffers\n");
            return;
        }
        dma_buffers_phys[i] = (uint32_t)v2p((uint64_t)dma_buffers[i]);
        memset(dma_buffers[i], 0, 16384);

        bdl[i].addr   = dma_buffers_phys[i];
        bdl[i].length = 0;
        bdl[i].flags  = 0;
    }

    write_idx = 0;
    sem_init(&mixer_sem, 0);
    ac97_found = true;

    for (int i = 0; i < MAX_AUDIO_CLIENTS; i++) {
        audio_clients[i].active = false;
        audio_clients[i].buffer = NULL;
        audio_clients[i].lock   = SPINLOCK_INIT;
        wait_queue_init(&audio_clients[i].write_waitq);
        wait_queue_init(&audio_clients[i].sync_waitq);
    }

    uint32_t intr = pci_read_config(ac97_dev.bus, ac97_dev.device, ac97_dev.function, 0x3C);
    uint8_t irq = intr & 0xFF;
    serial_write("[AC97] PCI Interrupt Line (IRQ): ");
    serial_write_num(irq);
    serial_write("\n");

    if (irq != 0 && irq != 0xFF) {
        // Register C-level handler in the generic IRQ dispatch table
        extern void idt_register_irq_handler(int irq, uint64_t (*handler)(registers_t *regs));
        idt_register_irq_handler(irq, ac97_handler);

        // Unmask IRQ on the PIC
        if (irq < 8) {
            outb(0x21, inb(0x21) & ~(1 << irq));
        } else {
            outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
        }

        serial_write("[AC97] Registered IRQ handler on IRQ ");
        serial_write_num(irq);
        serial_write("\n");
    } else {
        serial_write("[AC97] Warning: No valid IRQ found in PCI configuration\n");
    }

    process_t *mixer_proc = process_create(ac97_mixer_thread, false);
    if (!mixer_proc) {
        serial_write("[AC97] Failed to create mixer thread\n");
    } else {
        serial_write("[AC97] Mixer thread spawned successfully\n");
    }

    serial_write("[AC97] Initialization successful! Variable rate audio: ");
    serial_write(vra_supported ? "yes\n" : "no\n");
}

void *ac97_open_client(void) {
    uint64_t flags = spinlock_acquire_irqsave(&mixer_lock);
    for (int i = 0; i < MAX_AUDIO_CLIENTS; i++) {
        if (!audio_clients[i].active) {
            audio_clients[i].active    = true;
            audio_clients[i].speed     = 48000;
            audio_clients[i].channels  = 2;
            audio_clients[i].format    = AFMT_S16_LE;
            audio_clients[i].write_pos = 0;
            audio_clients[i].read_pos  = 0;
            audio_clients[i].count     = 0;
            audio_clients[i].phase     = 0;
            if (!audio_clients[i].buffer) {
                // Buffer is allocated once and persists for subsequent opens to prevent allocation overhead
                audio_clients[i].buffer = (uint8_t *)kmalloc(CLIENT_BUFFER_SIZE);
            }
            memset(audio_clients[i].buffer, 0, CLIENT_BUFFER_SIZE);
            spinlock_release_irqrestore(&mixer_lock, flags);
            return &audio_clients[i];
        }
    }
    spinlock_release_irqrestore(&mixer_lock, flags);
    return NULL;
}

void ac97_close_client(void *handle) {
    audio_client_t *client = (audio_client_t *)handle;
    if (!client) return;
    uint64_t flags   = spinlock_acquire_irqsave(&mixer_lock);
    uint64_t c_flags = spinlock_acquire_irqsave(&client->lock);
    client->active = false;
    client->count  = 0;
    spinlock_release_irqrestore(&client->lock, c_flags);
    spinlock_release_irqrestore(&mixer_lock, flags);
    wait_queue_wake_all(&client->write_waitq);
    wait_queue_wake_all(&client->sync_waitq);
}

int ac97_write(void *handle, const void *buf, int size) {
    audio_client_t *client = (audio_client_t *)handle;
    if (!ac97_found || !client || !client->active) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    int written = 0;

    while (written < size) {
        uint64_t flags = spinlock_acquire_irqsave(&client->lock);
        if (!client->active) {
            spinlock_release_irqrestore(&client->lock, flags);
            return -1;
        }

        uint32_t free_space = CLIENT_BUFFER_SIZE - client->count;
        if (free_space == 0) {
            wait_queue_entry_t entry;
            entry.proc = process_get_current();
            entry.next = NULL;
            // Race Prevention: Add to wait queue under lock *before* checking condition/releasing lock.
            // Any concurrent wakeup will see us in the queue and set us to RUNNING.
            wait_queue_add(&client->write_waitq, &entry);

            entry.proc->state = PROC_STATE_BLOCKED;
            spinlock_release_irqrestore(&client->lock, flags);

            sched_yield();

            // Remove the stack-allocated entry to avoid dangling pointers in the queue.
            wait_queue_remove(&client->write_waitq, &entry);
            continue;
        }

        uint32_t to_write = size - written;
        if (to_write > free_space) to_write = free_space;

        // Write into the ring buffer, handling the wraparound at the end.
        uint32_t space_to_end = CLIENT_BUFFER_SIZE - client->write_pos;
        if (to_write <= space_to_end) {
            memcpy(client->buffer + client->write_pos, src + written, to_write);
            client->write_pos = (client->write_pos + to_write) % CLIENT_BUFFER_SIZE;
        } else {
            memcpy(client->buffer + client->write_pos, src + written, space_to_end);
            memcpy(client->buffer, src + written + space_to_end, to_write - space_to_end);
            client->write_pos = to_write - space_to_end;
        }

        client->count += to_write;
        spinlock_release_irqrestore(&client->lock, flags);

        sem_signal(&mixer_sem);

        written += to_write;
    }

    return written;
}

int ac97_read(void *handle, void *buf, int size) {
    (void)handle;
    (void)buf;
    (void)size;
    return 0; // Recording not implemented
}

int ac97_dsp_ioctl(void *handle, uint64_t request, void *arg) {
    if (!ac97_found) return -1;
    audio_client_t *client = (audio_client_t *)handle;
    if (!client || !client->active) return -1;

    switch (request) {
        case SNDCTL_DSP_SPEED: {
            if (!arg) return -EINVAL;
            int rate = *(int*)arg;
            uint64_t flags = spinlock_acquire_irqsave(&client->lock);
            client->speed = rate;
            spinlock_release_irqrestore(&client->lock, flags);
            *(int*)arg = rate;
            return 0;
        }
        case SNDCTL_DSP_CHANNELS: {
            if (!arg) return -EINVAL;
            uint64_t flags = spinlock_acquire_irqsave(&client->lock);
            client->channels = *(int*)arg;
            spinlock_release_irqrestore(&client->lock, flags);
            *(int*)arg = 2; 
            return 0;
        }
        case SNDCTL_DSP_SETFMT: {
            if (!arg) return -EINVAL;
            int fmt = *(int*)arg;
            if (fmt != AFMT_S16_LE) {
                *(int*)arg = AFMT_S16_LE;
                return -EINVAL;
            }
            uint64_t flags = spinlock_acquire_irqsave(&client->lock);
            client->format = AFMT_S16_LE;
            spinlock_release_irqrestore(&client->lock, flags);
            return 0;
        }
        case SNDCTL_DSP_RESET: {
            uint64_t flags = spinlock_acquire_irqsave(&client->lock);
            client->write_pos = 0;
            client->read_pos  = 0;
            client->count     = 0;
            client->phase     = 0;
            spinlock_release_irqrestore(&client->lock, flags);
            return 0;
        }
        case SNDCTL_DSP_SYNC: {
            // Block until the mixer has consumed all buffered data.
            while (true) {
                uint64_t flags = spinlock_acquire_irqsave(&client->lock);
                if (!client->active) {
                    spinlock_release_irqrestore(&client->lock, flags);
                    return -EINVAL;
                }
                uint32_t count = client->count;
                if (count == 0) {
                    spinlock_release_irqrestore(&client->lock, flags);
                    break;
                }

                wait_queue_entry_t entry;
                entry.proc = process_get_current();
                entry.next = NULL;
                // Race Prevention: Add to wait queue under lock *before* checking condition/releasing lock.
                wait_queue_add(&client->sync_waitq, &entry);

                entry.proc->state = PROC_STATE_BLOCKED;
                spinlock_release_irqrestore(&client->lock, flags);

                sched_yield();

                // Remove the stack-allocated entry to avoid dangling pointers in the queue.
                wait_queue_remove(&client->sync_waitq, &entry);
            }
            return 0;
        }
        case SNDCTL_DSP_STEREO: {
            if (!arg) return -1;
            uint64_t flags = spinlock_acquire_irqsave(&client->lock);
            client->channels = *(int*)arg ? 2 : 1;
            spinlock_release_irqrestore(&client->lock, flags);
            *(int*)arg = 1; // Report stereo; mono upmixing is not implemented
            return 0;
        }
    }
    return -1;
}

int ac97_mixer_ioctl(uint64_t request, void *arg) {
    if (!ac97_found) return -1;

    // AC97 volume registers store attenuation in 5-bit steps (0 = max, 31 = min).
    // Bit 15 is the mute flag. The OSS interface uses 0-100 percentage values
    // packed as left | (right << 8) for stereo controls.

    switch (request) {
        case SOUND_MIXER_READ_VOLUME: {
            if (!arg) return -1;
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            uint16_t val = inw(nam_base + 0x02); // NAM+0x02: Master Volume
            spinlock_release_irqrestore(&ac97_lock, flags);

            int vol;
            if (val & 0x8000) {
                vol = 0; // Muted
            } else {
                int step_L = (val >> 8) & 0x1F;
                int step_R =  val       & 0x1F;
                int vol_L  = (31 - step_L) * 100 / 31;
                int vol_R  = (31 - step_R) * 100 / 31;
                vol = vol_L | (vol_R << 8);
            }
            *(int*)arg = vol;
            return 0;
        }
        case SOUND_MIXER_WRITE_VOLUME: {
            if (!arg) return -1;
            int vol   = *(int*)arg;
            int vol_L =  vol        & 0xFF;
            int vol_R = (vol >> 8)  & 0xFF;

            // Update software volume so the mixer thread applies it immediately.
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            sw_vol_l = vol_L;
            sw_vol_r = vol_R;

            uint16_t val;
            if (vol_L == 0 && vol_R == 0) {
                val = 0x8000 | (31 << 8) | 31; // Mute with full attenuation
            } else {
                int step_L = 31 - (vol_L * 31 / 100);
                int step_R = 31 - (vol_R * 31 / 100);
                val = ((step_L & 0x1F) << 8) | (step_R & 0x1F);
            }
            outw(nam_base + 0x02, val);
            spinlock_release_irqrestore(&ac97_lock, flags);
            return 0;
        }
        case SOUND_MIXER_READ_PCM: {
            if (!arg) return -1;
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            uint16_t val = inw(nam_base + 0x18); // NAM+0x18: PCM Out Volume
            spinlock_release_irqrestore(&ac97_lock, flags);

            int vol;
            if (val & 0x8000) {
                vol = 0;
            } else {
                int step_L = (val >> 8) & 0x1F;
                int step_R =  val       & 0x1F;
                int vol_L  = (31 - step_L) * 100 / 31;
                int vol_R  = (31 - step_R) * 100 / 31;
                vol = vol_L | (vol_R << 8);
            }
            *(int*)arg = vol;
            return 0;
        }
        case SOUND_MIXER_WRITE_PCM: {
            if (!arg) return -1;
            int vol   = *(int*)arg;
            int vol_L =  vol        & 0xFF;
            int vol_R = (vol >> 8)  & 0xFF;

            // Update software PCM volume alongside the hardware register.
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            sw_pcm_l = vol_L;
            sw_pcm_r = vol_R;

            uint16_t val;
            if (vol_L == 0 && vol_R == 0) {
                val = 0x8000 | (31 << 8) | 31;
            } else {
                int step_L = 31 - (vol_L * 31 / 100);
                int step_R = 31 - (vol_R * 31 / 100);
                val = ((step_L & 0x1F) << 8) | (step_R & 0x1F);
            }
            outw(nam_base + 0x18, val);
            spinlock_release_irqrestore(&ac97_lock, flags);
            return 0;
        }
        case SOUND_MIXER_READ_MIC: {
            // NAM+0x0E: MIC Volume (mono, 5-bit attenuation only).
            if (!arg) return -1;
            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            uint16_t val = inw(nam_base + 0x0E);
            spinlock_release_irqrestore(&ac97_lock, flags);

            int vol;
            if (val & 0x8000) {
                vol = 0;
            } else {
                int step = val & 0x1F;
                vol = (31 - step) * 100 / 31;
            }
            *(int*)arg = vol;
            return 0;
        }
        case SOUND_MIXER_WRITE_MIC: {
            if (!arg) return -1;
            int vol = *(int*)arg;

            uint16_t val;
            if (vol == 0) {
                val = 0x8000 | 31;
            } else {
                int step = 31 - (vol * 31 / 100);
                val = step & 0x1F;
            }

            uint64_t flags = spinlock_acquire_irqsave(&ac97_lock);
            outw(nam_base + 0x0E, val);
            spinlock_release_irqrestore(&ac97_lock, flags);
            return 0;
        }
    }
    return -1;
}
