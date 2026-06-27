// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#include "kutils.h"
#include "pcsk.h"

int pcsk_ioctl(void *file_handle, uint64_t request, void *arg) {
    (void)file_handle;
    if (request == PCSK_IOCTL_BEEP) {
        if (!arg) return -1;
        struct pcsk_beep *b = (struct pcsk_beep *)arg;
        // Validate parameters
        if (b->ms == 0) {
            // treat 0 as stop
            k_beep(0, 0);
            return 0;
        }
        if (b->freq == 0) return -1;
        k_beep((int)b->freq, (int)b->ms);
        return 0;
    }
    return -1;
}
