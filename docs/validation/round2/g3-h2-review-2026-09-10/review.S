.intel_syntax noprefix
.text
.globl fixture_after_syscall
.globl fixture_after_syscall_end
fixture_after_syscall:
    syscall
    mov qword ptr [r12], 42
    movabs r15, 0x1122334455667788
    jmp r15
fixture_after_syscall_end:
