; Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
section .text
global fpu_save_to
global fpu_restore_from

; void fpu_save_to(uint8_t *dst)
fpu_save_to:
    fxsave [rdi]
    ret

; void fpu_restore_from(const uint8_t *src)
fpu_restore_from:
    fxrstor [rdi]
    ret
