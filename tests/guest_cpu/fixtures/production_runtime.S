// SPDX-License-Identifier: GPL-2.0-or-later
// ELF assembled by generate-production-runtime-fixture; no game bytes.
.intel_syntax noprefix
.text
.global _start
_start:
#ifdef RUNTIME_DEPENDENCY
    ret
    .org 0x100
dependency_init:
    cmp qword ptr [rip + tls_relocations], 2
    jne dependency_bad
#ifdef WAIT_FOREVER
    sub rsp, 8
dependency_wait:
    mov edi, 500000
    call qword ptr [rip + got_sleep]
    jmp dependency_wait
#endif
    cmp qword ptr fs:[-128], 0x5678
    jne dependency_bad
    inc qword ptr fs:[-128]
    xor eax, eax
    ret
dependency_bad:
    mov eax, -1
    ret
#else
    mov r12, rsi
    sub rsp, 8
#ifdef WAIT_FOREVER
wait_loop:
    mov edi, 500000
    call qword ptr [rip + got_sleep]
    jmp wait_loop
#else
    mov rax, qword ptr [rip + got_stack_guard]
    cmp qword ptr [rax], 0
    je failed
    mov rax, qword ptr [rip + got_progname]
    mov rax, qword ptr [rax]
    cmp byte ptr [rax], 0
    je failed
    mov rax, qword ptr [rip + got_environ]
    mov rax, qword ptr [rax]
    cmp qword ptr [rax], 0
    jne failed
    cmp qword ptr [rip + tls_relocations], 1
    jne failed
    cmp qword ptr [rip + tls_relocations + 8], 0
    jne failed
    cmp qword ptr [rip + tls_relocations + 16], -64
    jne failed
    mov rax, qword ptr fs:[-64]
    cmp rax, 0x1234
    jne failed
#ifdef EXPECT_DEPENDENCY
    lea rdi, [rip + dependency_tls_index]
    call qword ptr [rip + got_tls]
    cmp qword ptr [rax], 0x5679
    jne failed
#endif
    call qword ptr [rip + got_errno]
    cmp dword ptr [rax], 0
    jne failed
    mov edi, 1000000
    call qword ptr [rip + got_sleep]
    cmp eax, -1
    jne failed
    call qword ptr [rip + got_errno]
    cmp dword ptr [rax], 22
    jne failed
    lea rdi, [rip + child_handle]
#ifdef BAD_POINTER
    mov edi, 1
#endif
    xor esi, esi
    lea rdx, [rip + child_entry]
    mov ecx, 0x42
    call qword ptr [rip + got_create]
    test eax, eax
    jne failed
child_started:
    cmp qword ptr [rip + child_ready], 10000
    jb child_started
    lea rdi, [rip + allocated]
    mov esi, 0x4000
    mov edx, 3
    xor ecx, ecx
    lea r8, [rip + map_name]
    call qword ptr [rip + got_map]
    test eax, eax
    jne failed
    mov rdi, qword ptr [rip + allocated]
    mov qword ptr [rdi], 0x5566
    mov esi, 0x4000
    mov edx, 1
    call qword ptr [rip + got_protect]
    test eax, eax
    jne failed
    mov rdi, qword ptr [rip + allocated]
    cmp qword ptr [rdi], 0x5566
    jne failed
    mov esi, 0x4000
    call qword ptr [rip + got_unmap]
    test eax, eax
    jne failed
    mov qword ptr [rip + child_release], 1
    mov rdi, qword ptr [rip + child_handle]
    lea rsi, [rip + child_result]
    call qword ptr [rip + got_join]
    test eax, eax
    jne failed
    cmp qword ptr [rip + child_result], 0x1276
    jne failed
    cmp qword ptr fs:[-64], 0x1234
    jne failed
    call qword ptr [rip + got_errno]
    cmp dword ptr [rax], 22
    jne failed
    lea rdi, [rip + once_word]
    lea rsi, [rip + once_callback]
    call qword ptr [rip + got_once]
    test eax, eax
    jne failed
    cmp qword ptr fs:[-64], 0x1235
    jne failed
    mov eax, 0xcafe
    jmp done
failed:
    mov eax, 0xbad
done:
    mov qword ptr [rip + marker], rax
    add rsp, 8
    jmp r12
child_entry:
    push rbx
    mov rbx, rdi
    call qword ptr [rip + got_errno]
    cmp dword ptr [rax], 0
    jne child_bad
    mov dword ptr [rax], 88
child_spin:
    inc qword ptr [rip + child_ready]
    cmp qword ptr [rip + child_release], 1
    jne child_spin
    mov rax, qword ptr fs:[-64]
    add rax, rbx
    mov qword ptr fs:[-64], 0x7777
    pop rbx
    ret
child_bad:
    mov eax, 0xbad
    pop rbx
    ret
once_callback:
    sub rsp, 8
    lea rdi, [rip + tls_index]
    call qword ptr [rip + got_tls]
    add qword ptr [rax], 1
    lea rdi, [rip + allocated]
    mov esi, 0x4000
    mov edx, 3
    xor ecx, ecx
    lea r8, [rip + map_name]
    call qword ptr [rip + got_map]
    test eax, eax
    jne failed
    mov rdi, qword ptr [rip + allocated]
    mov qword ptr [rdi], 0x5566
    mov esi, 0x4000
    mov edx, 1
    call qword ptr [rip + got_protect]
    test eax, eax
    jne failed
    mov rdi, qword ptr [rip + allocated]
    cmp qword ptr [rdi], 0x5566
    jne failed
    mov esi, 0x4000
    call qword ptr [rip + got_unmap]
    test eax, eax
    jne failed
    add rsp, 8
    ret
#endif
#endif
.org 0x4000
marker: .quad 0
.org 0x4010
tls_image:
#ifdef RUNTIME_DEPENDENCY
    .quad 0x5678
#else
    .quad 0x1234
#endif
.org 0x4100
proc_param:
    .quad 0x20
    .long 0x4942524f, 1
    .quad 0x09000000, 0
.org 0x4200
child_handle: .quad 0
child_result: .quad 0
once_word: .quad 0
allocated: .quad 0
tls_index: .quad 1, 0
dependency_tls_index: .quad 2, 0
map_name: .asciz "ProductionFixture"
.balign 8
child_ready: .quad 0
child_release: .quad 0
 .org 0x4300
tls_relocations: .quad 0, 0, 0
.org 0x8000
got_create: .quad 0
got_join: .quad 0
got_once: .quad 0
got_tls: .quad 0
got_map: .quad 0
got_protect: .quad 0
got_unmap: .quad 0
got_sleep: .quad 0
got_errno: .quad 0
got_stack_guard: .quad 0
got_progname: .quad 0
got_environ: .quad 0
.org 0xc000
