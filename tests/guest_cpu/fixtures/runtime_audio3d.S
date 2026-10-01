// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12, rsi
    sub rsp, 24
    mov edi, 0xa7
    call qword ptr [rip + got_load]
    test eax, eax
    jne failed
    xor edi, edi
    call qword ptr [rip + got_init]
    test eax, eax
    jne failed
    lea rdi, [rip + params]
    call qword ptr [rip + got_defaults]
    test eax, eax
    jne failed
    mov edi, 255
    lea rsi, [rip + params]
    lea rdx, [rip + port]
    call qword ptr [rip + got_port_open]
    test eax, eax
    jne failed
    mov edi, [rip + port]
    mov esi, 255
    xor edx, edx
    xor ecx, ecx
    mov r8d, 256
    mov r9d, 48000
    // The seventh argument must be read from guest RSP+8 at the call boundary.
    // A missing stack read would select valid S16 mono and open a real device.
    mov qword ptr [rsp], 255
    call qword ptr [rip + got_audio_open]
    cmp eax, 0x80260007 // AudioOut INVALID_FORMAT
    jne failed
    mov edi, [rip + port]
    call qword ptr [rip + got_port_close]
    test eax, eax
    jne failed
    call qword ptr [rip + got_term]
    test eax, eax
    jne failed
    mov edi, 0xa7
    call qword ptr [rip + got_unload]
    test eax, eax
    jne failed
    mov eax, 0xcafe
    jmp done
failed:
    mov eax, 0xbad
done:
    add rsp, 24
    jmp r12
.org 0x4010
    .quad 0x1234
.org 0x4100
    .quad 0x20
    .long 0x4942524f, 1
    .quad 0x09000000, 0
.org 0x4400
params: .zero 40
port: .long 0
.org 0x8000
got_init: .quad 0
got_defaults: .quad 0
got_port_open: .quad 0
got_audio_open: .quad 0
got_port_close: .quad 0
got_term: .quad 0
got_load: .quad 0
got_unload: .quad 0
