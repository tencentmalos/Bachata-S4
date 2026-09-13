// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic provider-selection probe, not an implementation of a system libc.
.intel_syntax noprefix
.text
.global _start
_start:
#ifdef RUNTIME_DEPENDENCY
    ret
.org 0x100
init:
    cmp dword ptr [rip+init_flags],2
    jne bad
    inc dword ptr [rip+init_flags]
    xor eax,eax
    ret
bad:
    mov eax,-1
    ret
.org 0x180
malloc_init:
#ifdef BAD_POINTER
    jmp bad
#endif
    cmp dword ptr [rip+init_flags],0
    jne bad
    inc dword ptr [rip+init_flags]
    xor eax,eax
    ret
.org 0x200
mutex_enable:
    cmp dword ptr [rip+init_flags],1
    jne bad
    inc dword ptr [rip+init_flags]
    ret
.org 0x280
length:
    cmp dword ptr [rip+init_flags],3
    jne bad
    xor eax,eax
length_loop:
    cmp byte ptr [rdi+rax],0
    je length_done
    inc rax
    jmp length_loop
length_done:
    // Distinct result tags let the test prove which provider executed.
#ifdef SYSTEM_LIBC
    add eax,0x200
#else
    add eax,0x100
#endif
    ret
#else
    mov r12,rsi
    sub rsp,8
    lea rdi,[rip+text]
    call qword ptr [rip+got_length]
    add rsp,8
    jmp r12
#endif
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
init_flags:.long 0
text:.asciz "libc"
.org 0x4300
.quad 0,0,0
.org 0x8000
got_length:.quad 0
