// Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef FPU_H
#define FPU_H

#include <stdint.h>

void fpu_save_to(uint8_t *dst);
void fpu_restore_from(const uint8_t *src);

static inline void fpu_switch(uint64_t from_rsp, uint64_t to_rsp) {
    fpu_save_to((uint8_t *)from_rsp);
    fpu_restore_from((const uint8_t *)to_rsp);
}

#endif
