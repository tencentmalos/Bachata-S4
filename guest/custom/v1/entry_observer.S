/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Expanded by the builder with audited entry, callback and trampoline symbols.
 * Preserve the original red zone even for leaf/handwritten callers. No return
 * address replacement or TLS shadow stack is needed: original runs as a tailcall.
 * AVX is an explicit adapter requirement (PS4/FEX), not a generic x86 claim. */
.macro SHAD_ENTRY_OBSERVER entry, callback, original
.text
.p2align 4
.globl \entry
.type \entry,@function
\entry:
    lea -128(%rsp), %rsp
    pushfq
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax
    mov %rsp, %r12
    and $-32, %rsp
    sub $1088, %rsp
    fxsave64 (%rsp)
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vmovdqu %ymm\n, (512 + 32 * \n)(%rsp)
    .endr
    fninit
    movl $0x1f80, 1024(%rsp)
    ldmxcsr 1024(%rsp)
    cld
    lea 256(%r12), %rax
    mov %rax, 1032(%rsp)
    mov %r12, 1040(%rsp)
    mov %rsp, 1048(%rsp)
    lea 512(%rsp), %rax
    mov %rax, 1056(%rsp)
    lea 1032(%rsp), %rdi
    call \callback
    fxrstor64 (%rsp)
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    vmovdqu (512 + 32 * \n)(%rsp), %ymm\n
    .endr
    mov %r12, %rsp
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    popfq
    lea 128(%rsp), %rsp
    jmp \original
.size \entry, .-\entry
.endm
