// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    xor ebp,ebp
    call outer
    mov eax,0xcafe
    add rsp,8
    jmp r12
outer:
    push rbp
    mov rbp,rsp
    call inner
    pop rbp
    ret
inner:
    push rbp
    mov rbp,rsp
    lea rdi,[rip+prefix]
    call qword ptr [rip+got_print]
    // An unreadable diagnostic prefix must not turn into a native pointer.
    mov rdi,-1
    call qword ptr [rip+got_print]
    pop rbp
    ret
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4400
prefix:.asciz "backtrace-fixture"
.org 0x8000
got_print:.quad 0
