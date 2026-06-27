; Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
global process_jump_usermode

section .text


process_jump_usermode:
    cli

    ; Load user data segment (0x23)
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build the IRETQ stack frame
    ; 1. SS (User Data Segment)
    push 0x1B
    
    ; 2. RSP (User Stack)
    push rsi
    
    ; 3. RFLAGS (Enable Interrupts: IF = 0x200 | Reserved bit 1 = 0x2 -> 0x202)
    push 0x202
    
    ; 4. CS (User Code Segment)
    push 0x23
    
    ; 5. RIP (Entry Point)
    push rdi

    ; Jump to Ring 3!
    iretq

; void context_switch_to(uint64_t rsp)
; Restores context from isr frame and jumps
global context_switch_to
context_switch_to:
    mov rsp, rdi
    xor ecx, ecx
    xgetbv
    xrstor [rsp]
    add rsp, 8192
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    
    add rsp, 16 ; drop int_no and err_code
    iretq
