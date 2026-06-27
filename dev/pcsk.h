// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef SRC_DEV_PCSK_H
#define SRC_DEV_PCSK_H

#include <stdint.h>

#define PCSK_IOCTL_BEEP 0x5001

struct pcsk_beep {
    uint32_t freq; /* Hz */
    uint32_t ms;   /* milliseconds */
};

#endif
