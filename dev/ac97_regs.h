// Copyright (c) 2026 Zerfithel (https://github.com/zerfithel)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
#ifndef AC97_REGS_H
#define AC97_REGS_H

#define BUFFER_FLAG_INTERRUPT_ON_COMPLETE (0x8000)

#define AC97_VOLUME_MUTE (0x8000)

#define AC97_VOLUME_BITS (5)
#define AC97_VOLUME_MAX ((1 << AC97_VOLUME_BITS) - 1)

#define AC97_VOL_PERCENT_MAX (100)
#define AC97_LEFT_SHIFT (8)

#endif
