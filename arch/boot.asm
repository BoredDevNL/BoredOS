; 64-bit Entry Point for BoredOS
; Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
section .text
global _start
extern kmain

bits 64

_start:
    cli
    

    call kmain

    hlt
.loop:
    jmp .loop
