// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stdbool.h>

// AC97 Buffer Descriptor List entry. The hardware DMA engine reads an array of
// these to find the next buffer to play. Must be packed to match the hardware layout.
typedef struct {
    uint32_t addr;   // Physical address of the audio buffer
    uint16_t length; // Number of 16-bit samples in the buffer
    uint16_t flags;  // BUP (bit 14) / IOC (bit 15) control bits
} __attribute__((packed)) ac97_bd_t;

// Initialize the AC97 controller: scans PCI, resets the codec, allocates DMA
// buffers and BDL, and spawns the mixer kernel thread.
void ac97_init(void);

// Returns true if an AC97 controller was found and successfully initialized.
bool ac97_present(void);

// Allocate a software audio client slot. Returns an opaque handle on success,
// or NULL if all MAX_AUDIO_CLIENTS slots are already in use.
void *ac97_open_client(void);

// Release a previously allocated client slot and silence its contribution to the mix.
void ac97_close_client(void *handle);

// Write PCM audio data into the client's ring buffer. Blocks (with brief sleeps)
// if the buffer is full. Returns the number of bytes written, or -1 on error.
int ac97_write(void *handle, const void *buf, int size);

// Audio input is not implemented yet, always returns 0 (EOF).
int ac97_read(void *handle, void *buf, int size);

// DSP-style ioctl for a specific client (sample rate, format, channel count, sync, reset).
int ac97_dsp_ioctl(void *handle, uint64_t request, void *arg);

// Global mixer ioctl for reading and writing hardware volume registers
// (master output, PCM output, microphone input).
int ac97_mixer_ioctl(uint64_t request, void *arg);

#endif // AC97_H
