; Copyright (c) 2023-2026 Christiaan (chris@boreddev.nl)
; This software is released under the GNU General Public License v3.0. See LICENSE file for details.
; This header needs to maintain in any file it is present in, as per the GPL license terms.
section .text
global isr0_wrapper
global isr1_wrapper
global isr8_wrapper
global isr12_wrapper
global isr14_wrapper
global isr128_wrapper
global isr_sched_ipi_wrapper
global isr5_wrapper
global isr9_wrapper
global isr10_wrapper
global isr11_wrapper
extern timer_handler
extern keyboard_handler
extern mouse_handler
extern sched_ipi_handler
extern syscall_handler_c
extern exception_handler_c
extern ac97_handler
extern pci_irq_handler

; Helper to send EOI (End of Interrupt) to PIC
send_eoi:
    push rax
    mov al, 0x20
    out 0x20, al ; Master PIC

    pop rax
    ret

%macro ISR_NOERRCODE 2
isr%2_wrapper:
    push 0      ; Dummy error code
    push %2     ; Vector
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
    
    test qword [rsp + 144], 3
    jz %%skip_swap
    swapgs
%%skip_swap:
    
    sub rsp, 512

    ; Pass current RSP as 1st argument (registers_t*)
    mov rdi, rsp
    
    call %1
    
    ; Update RSP with return value (task switch)
    mov rsp, rax

    add rsp, 512
    
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

    test qword [rsp + 24], 3
    jz %%skip_swap_back
    swapgs
%%skip_swap_back:

    add rsp, 16 ; drop dummy vector and error code
    iretq
%endmacro

isr0_wrapper:
    ISR_NOERRCODE timer_handler, 32

isr1_wrapper:
    ISR_NOERRCODE keyboard_handler, 33

isr12_wrapper:
    ISR_NOERRCODE mouse_handler, 44

isr5_wrapper:
    ISR_NOERRCODE ac97_handler, 37

isr9_wrapper:
    ISR_NOERRCODE ac97_handler, 41

isr10_wrapper:
    ISR_NOERRCODE ac97_handler, 42

isr11_wrapper:
    ISR_NOERRCODE ac97_handler, 43

isr_sched_ipi_wrapper:
    ISR_NOERRCODE sched_ipi_handler, 65

isr128_wrapper:
    ISR_NOERRCODE syscall_handler_c, 128

; Common exception macro for exceptions WITHOUT error code
%macro EXCEPTION_NOERRCODE 1
global exc%1_wrapper
exc%1_wrapper:
    push 0      ; Dummy error code
    push %1     ; Vector
    jmp exception_common
%endmacro

; Common exception macro for exceptions WITH error code
%macro EXCEPTION_ERRCODE 1
global exc%1_wrapper
exc%1_wrapper:
    push %1     ; Vector
    jmp exception_common
%endmacro

; Define all 32 standard exceptions
EXCEPTION_NOERRCODE 0  ; Divide Error
EXCEPTION_NOERRCODE 1  ; Debug
EXCEPTION_NOERRCODE 2  ; NMI
EXCEPTION_NOERRCODE 3  ; Breakpoint
EXCEPTION_NOERRCODE 4  ; Overflow
EXCEPTION_NOERRCODE 5  ; Bound Range Exceeded
EXCEPTION_NOERRCODE 6  ; Invalid Opcode
EXCEPTION_NOERRCODE 7  ; Device Not Available
EXCEPTION_ERRCODE   8  ; Double Fault
EXCEPTION_NOERRCODE 9  ; Coprocessor Segment Overrun
EXCEPTION_ERRCODE   10 ; Invalid TSS
EXCEPTION_ERRCODE   11 ; Segment Not Present
EXCEPTION_ERRCODE   12 ; Stack-Segment Fault
EXCEPTION_ERRCODE   13 ; General Protection Fault
EXCEPTION_ERRCODE   14 ; Page Fault
EXCEPTION_NOERRCODE 15 ; Reserved
EXCEPTION_NOERRCODE 16 ; x87 Floating-Point Exception
EXCEPTION_ERRCODE   17 ; Alignment Check
EXCEPTION_NOERRCODE 18 ; Machine Check
EXCEPTION_NOERRCODE 19 ; SIMD Floating-Point Exception
EXCEPTION_NOERRCODE 20 ; Virtualization Exception
EXCEPTION_ERRCODE   21 ; Control Protection Exception
EXCEPTION_NOERRCODE 22 ; Reserved
EXCEPTION_NOERRCODE 23 ; Reserved
EXCEPTION_NOERRCODE 24 ; Reserved
EXCEPTION_NOERRCODE 25 ; Reserved
EXCEPTION_NOERRCODE 26 ; Reserved
EXCEPTION_NOERRCODE 27 ; Reserved
EXCEPTION_NOERRCODE 28 ; Hypervisor Injection Exception
EXCEPTION_ERRCODE   29 ; VMM Communication Exception
EXCEPTION_ERRCODE   30 ; Security Exception
EXCEPTION_NOERRCODE 31 ; Reserved

exception_common:
    ; Save registers (registers_t structure)
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

    test qword [rsp + 144], 3
    jz .skip_swap_exc
    swapgs
.skip_swap_exc:
    
    sub rsp, 512

    ; Pass current RSP as 1st argument (registers_t*)
    mov rdi, rsp
    
    call exception_handler_c
    
    ; Switch stack if needed (for process termination)
    mov rsp, rax

    add rsp, 512

    ; Restore registers
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

    test qword [rsp + 24], 3
    jz .skip_swap_back_exc
    swapgs
.skip_swap_back_exc:

    add rsp, 16 ; drop vector and error code
    iretq

