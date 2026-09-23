// SPDX-License-Identifier: GPL-2.0-or-later
// Runs against a user-supplied SystemGesture Guest module, never a host stub.
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    xor edi,edi
    call qword ptr [rip+got_sdk]
    cmp eax,0x80020016
    jne failed
    mov edi,1
    call qword ptr [rip+got_sdk]
    cmp eax,0x8002000e
    jne failed
    lea rdi,[rip+version]
    call qword ptr [rip+got_sdk]
    test eax,eax
    jne failed
    cmp dword ptr [rip+version],0x09000000
    jne failed
    call qword ptr [rip+got_gpi]
    test rax,rax
    jne failed
    call qword ptr [rip+got_fini]
    test eax,eax
    jne failed
    xor edi,edi
    xor esi,esi
    call qword ptr [rip+got_open]
    cmp eax,0x80890003
    jne failed
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    call qword ptr [rip+got_init]
    test eax,eax
    jne failed
    mov edi,1
    xor esi,esi
    call qword ptr [rip+got_open]
    cmp eax,0x80890001
    jne failed
    xor edi,edi
    xor esi,esi
    call qword ptr [rip+got_open]
    test eax,eax
    js failed
    mov r13d,eax
    mov edi,eax
    call qword ptr [rip+got_close]
    test eax,eax
    jne failed
    mov edi,r13d
    call qword ptr [rip+got_close]
    test eax,eax
    jns failed
    call qword ptr [rip+got_fini]
    test eax,eax
    jne failed
    call qword ptr [rip+got_fini]
    test eax,eax
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x40
.long 0x4942524f,1
.quad 0x09000000,0
.quad 0,0,0
.quad 0 // relocated pointer to libc parameters before DT_INIT
.org 0x4400
version: .long 0
.org 0x4500
// Game libc requires this versioned block even with its default allocator.
.quad 0x40
.long 1,0
.quad 0,0,0,0,0,0
.org 0x8000
got_init: .quad 0
got_fini: .quad 0
got_open: .quad 0
got_close: .quad 0
got_sdk: .quad 0
got_gpi: .quad 0
