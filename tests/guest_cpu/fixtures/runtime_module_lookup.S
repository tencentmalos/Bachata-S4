// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
.macro load path
    lea rdi,[rip+\path]
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    xor r8d,r8d
    lea r9,[rip+result]
    call qword ptr [rip+got_load]
.endm
_start:
#ifdef RUNTIME_DEPENDENCY
module_init:
    inc dword ptr [rip+init_count]
    xor eax,eax
    ret
.org 0x100
    jmp module_init
#else
    mov r12,rsi
    sub rsp,8
    load relative
    test eax,eax
    jne failed
    load absolute
    test eax,eax
    jne failed
    cmp dword ptr [rip+result],0x12345678
    jne failed
    load missing
    cmp eax,0x80020002
    jne failed
    load escape
    cmp eax,0x8002000d
    jne failed
    lea rdi,[rip+relative]
    xor esi,esi
    xor edx,edx
    xor ecx,ecx
    xor r8d,r8d
    mov r9d,1
    call qword ptr [rip+got_load]
    cmp eax,0x8002000e
    jne failed
    xor edi,edi
    lea rsi,[rip+symbol]
    lea rdx,[rip+function]
    call qword ptr [rip+got_dlsym]
    test eax,eax
    jne failed
    call qword ptr [rip+function]
    cmp eax,0xcafe
    jne failed
    xor edi,edi
    lea rsi,[rip+symbol]
    mov edx,1
    call qword ptr [rip+got_dlsym]
    cmp eax,0x8002000e
    jne failed
    xor edi,edi
    lea rsi,[rip+missing]
    lea rdx,[rip+function]
    call qword ptr [rip+got_dlsym]
    cmp eax,0x80020003
    jne failed
    cmp qword ptr [rip+function],0
    jne failed
    load dependency
    cmp eax,1
    jne failed
    load dependency
    cmp eax,1
    jne failed
    cmp dword ptr [rip+result],0x12345678
    jne failed
    mov edi,1
    lea rsi,[rip+symbol]
    lea rdx,[rip+function]
    call qword ptr [rip+got_dlsym]
    test eax,eax
    jne failed
    call qword ptr [rip+function]
    cmp eax,0xcafe
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
#endif
.org 0x300
exported_value:
#ifdef RUNTIME_DEPENDENCY
#ifdef NO_INIT
    cmp dword ptr [rip+init_count],0
#else
    cmp dword ptr [rip+init_count],1
#endif
    jne init_bad
#endif
    mov eax,0xcafe
    ret
#ifdef RUNTIME_DEPENDENCY
init_bad:
    mov eax,0xbad
    ret
#endif
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4400
result:.long 0x12345678
function:.quad 0
init_count:.long 0
relative:.asciz "eboot.bin"
absolute:.asciz "/app0/eboot.bin"
missing:.asciz "/app0/does-not-exist.prx"
escape:.asciz "/app0/../outside.prx"
symbol:.asciz "_malloc_init"
dependency:.asciz "/app0/fixture_dependency.sprx"
.org 0x8000
got_load:.quad 0
got_dlsym:.quad 0
