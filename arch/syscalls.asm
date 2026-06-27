; Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
global syscall_entry
extern syscall_handler_c

section .text

; Syscall ABI:
; RAX = syscall_num
; RDI = arg1
; RSI = arg2
; RDX = arg3
; R10 = arg4
; R8  = arg5
; R9  = arg6

syscall_entry:
    swapgs 
    
    mov [gs:40], rsp
    mov rsp, [gs:48]

    ; 2. Build iretq frame 
    push 0x1B           ; SS (User Data)
    push qword [gs:40]  ; RSP
    push r11            ; RFLAGS (captured by syscall)
    push 0x23           ; CS (User Code)
    push rcx            ; RIP (return address from syscall)
    
    push 0              ; err_code
    push 0              ; int_no (can be used for syscall vector)
    
    ; 3. Save all registers in registers_t order
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Reserve fxsave_region slot 
    sub rsp, 512

    ; 4. Call C handler with registers_t*
    mov r12, rsp
    mov rdi, rsp
    call syscall_handler_c

    ; Check if task switch occurred (returned RSP in rax != input RSP in r12)
    cmp rax, r12
    jne .slow_path

    ; Fast path: no task switch. Return via sysretq!
    add rsp, 512       ; drop fxsave_region
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
    pop rcx            ; will be overwritten by sysretq anyway, but we pop it to restore GPRs
    pop rbx
    pop rax
    
    ; Extract RIP, RFLAGS, and RSP from the remaining frame:
    ; [rsp]     = int_no
    ; [rsp+8]   = err_code
    ; [rsp+16]  = rip
    ; [rsp+24]  = cs
    ; [rsp+32]  = rflags
    ; [rsp+40]  = rsp
    ; [rsp+48]  = ss
    mov rcx, [rsp + 16] ; user rip
    mov r11, [rsp + 32] ; user rflags
    mov rsp, [rsp + 40] ; user rsp
    swapgs
    o64 sysret

.slow_path:
    mov rsp, rax
    add rsp, 512

    ; 6. Restore and return via iretq
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
    add rsp, 16 ; drop int_no/err_code
    
    swapgs 
    iretq

section .bss
