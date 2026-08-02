// Copyright (c) 2026 Zerfithel (https://github.com/zerfithel)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
#ifndef AUDIO_H
#define AUDIO_H

#define AUDIO_CHANNELS (2)

#define AFMT_S16_LE (0x0010)
#define AFMT_U8     (0x0008)

#define BYTES_PER_FRAME (AUDIO_CHANNELS * sizeof(int16_t))

#endif
