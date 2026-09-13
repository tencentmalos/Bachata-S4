// SPDX-License-Identifier: GPL-2.0-or-later
.intel_syntax noprefix
.text
.global _start
_start:
    mov r12,rsi
    sub rsp,8
    lea rdi,[rip+key]
    lea rsi,[rip+key_dtor]
    call qword ptr [rip+got_key_create]
    test eax,eax
    jne failed
    lea rdi,[rip+thread_dtor]
#ifdef BAD_POINTER
    mov edi,1
#endif
    call qword ptr [rip+got_dtors]
    lea rdi,[rip+thread_dtor]
    call qword ptr [rip+got_count]
    lea rdi,[rip+thread_dtor]
    call qword ptr [rip+got_report]
    call qword ptr [rip+got_asan_new]
    test rax,rax
    jne failed
    call qword ptr [rip+got_asan_malloc]
    test rax,rax
    jne failed
    mov r13d,1
next_child:
    mov dword ptr [rip+order],0
    lea rdi,[rip+thread]
    xor esi,esi
    lea rdx,[rip+child]
    mov rcx,r13
    call qword ptr [rip+got_create]
    test eax,eax
    jne failed
    mov rdi,[rip+thread]
    lea rsi,[rip+result]
    call qword ptr [rip+got_join]
    test eax,eax
    jne failed
    cmp qword ptr [rip+result],0xabcd
    jne failed
    cmp [rip+dtor_count],r13d
    jne failed
    cmp [rip+key_count],r13d
    jne failed
    cmp dword ptr [rip+order],2
    jne failed
    inc r13d
    cmp r13d,3
    jne next_child
    xor edi,edi
    call qword ptr [rip+got_dtors]
    xor edi,edi
    call qword ptr [rip+got_count]
    xor edi,edi
    call qword ptr [rip+got_report]
    mov edi,[rip+key]
    call qword ptr [rip+got_key_delete]
    test eax,eax
    jne failed
    mov eax,0xcafe
    add rsp,8
    jmp r12
failed:
    mov eax,0xbad
    add rsp,8
    jmp r12
child:
    push rbx
    mov rbx,rdi
    mov edi,[rip+key]
    mov esi,0x1234
    call qword ptr [rip+got_key_set]
    test eax,eax
    jne child_bad
    cmp ebx,2
    jne child_return
    mov edi,0xabcd
    call qword ptr [rip+got_exit]
    // Explicit exit must not execute its successor.
child_bad:
    mov eax,0xbad
    pop rbx
    ret
child_return:
    mov eax,0xabcd
    pop rbx
    ret
thread_dtor:
    sub rsp,8
    mov edi,[rip+key]
    call qword ptr [rip+got_key_get]
    cmp eax,0x1234
    jne dtor_bad
    cmp dword ptr [rip+order],0
    jne dtor_bad
    inc dword ptr [rip+dtor_count]
    mov dword ptr [rip+order],1
    mov edi,[rip+key]
    mov esi,0x2345
    call qword ptr [rip+got_key_set]
    add rsp,8
    ret
dtor_bad:
    mov dword ptr [rip+order],0xbad
    add rsp,8
    ret
key_dtor:
    cmp edi,0x2345
    jne key_bad
    cmp dword ptr [rip+order],1
    jne key_bad
    inc dword ptr [rip+key_count]
    mov dword ptr [rip+order],2
    ret
key_bad:
    mov dword ptr [rip+order],0xbad
    ret
.org 0x4010
.quad 0x1234
.org 0x4100
.quad 0x20
.long 0x4942524f,1
.quad 0x09000000,0
.org 0x4200
key:.long 0
order:.long 0
dtor_count:.long 0
key_count:.long 0
thread:.quad 0
result:.quad 0
.org 0x8000
got_dtors:.quad 0
got_create:.quad 0
got_join:.quad 0
got_key_create:.quad 0
got_key_set:.quad 0
got_key_get:.quad 0
got_key_delete:.quad 0
got_exit:.quad 0
got_count:.quad 0
got_report:.quad 0
got_asan_new:.quad 0
got_asan_malloc:.quad 0
