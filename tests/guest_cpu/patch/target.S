.text
.p2align 4
.globl target_sum8
.type target_sum8,@function
target_sum8:
  push %rbp
  mov %rsp,%rbp
  nop
  lea (%rdi,%rsi),%rax
  add %rdx,%rax
  add %rcx,%rax
  add %r8,%rax
  add %r9,%rax
  add 16(%rbp),%rax
  add 24(%rbp),%rax
  pop %rbp
  ret
.size target_sum8,.-target_sum8
.p2align 4
.globl target_rip
.type target_rip,@function
target_rip:
  mov constant(%rip),%rax
  add %rdi,%rax
  ret
.size target_rip,.-target_rip
.p2align 4
.globl target_call
.type target_call,@function
target_call:
  call helper
  add %rdi,%rax
  ret
.size target_call,.-target_call
helper:
  mov $9,%eax
  ret
.p2align 4
.globl target_branch
.type target_branch,@function
target_branch:
  test %edi,%edi
  jz zero
  nop
  mov $7,%eax
  ret
zero:
  mov $3,%eax
  ret
.size target_branch,.-target_branch
.p2align 4
.globl target_float
.type target_float,@function
target_float:
  push %rbp
  mov %rsp,%rbp
  nop
  addsd %xmm1,%xmm0
  pop %rbp
  ret
.size target_float,.-target_float
.p2align 4
.globl target_sret
.type target_sret,@function
target_sret:
  push %rbp
  mov %rsp,%rbp
  nop
  mov %rsi,(%rdi)
  lea 1(%rsi),%rax
  mov %rax,8(%rdi)
  add $1,%rax
  mov %rax,16(%rdi)
  mov %rdi,%rax
  pop %rbp
  ret
.size target_sret,.-target_sret
.p2align 4
.globl target_varargs
.type target_varargs,@function
target_varargs:
  movzbl %al,%eax
  nop
  nop
  ret
.size target_varargs,.-target_varargs
.section .rodata
constant:.quad 17
.section .note.GNU-stack,"",@progbits
