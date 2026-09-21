
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 3c51de0: 58                           	popq	%rax
 3c51de1: ff ff                        	<unknown>
 3c51de3: ff c4                        	incl	%esp
 3c51de5: a1 78 11 84 33 9c 01 00 00   	movabsl	0x19c33841178, %eax
 3c51dee: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c51df2: c4 a1 78 11 84 33 b0 01 00 00	vmovups	%xmm0, 0x1b0(%rbx,%r14)
 3c51dfc: 42 c7 84 33 c0 01 00 00 00 00 00 00  	movl	$0x0, 0x1c0(%rbx,%r14)
 3c51e08: 42 89 8c 33 98 00 00 00      	movl	%ecx, 0x98(%rbx,%r14)
 3c51e10: 41 8d 4f ff                  	leal	-0x1(%r15), %ecx
 3c51e14: 46 89 84 33 9c 00 00 00      	movl	%r8d, 0x9c(%rbx,%r14)
 3c51e1c: 66 42 c7 84 33 94 00 00 00 01 00     	movw	$0x1, 0x94(%rbx,%r14)
 3c51e27: 42 c7 84 33 90 00 00 00 01 00 00 00  	movl	$0x1, 0x90(%rbx,%r14)
 3c51e33: 46 89 bc 33 a0 00 00 00      	movl	%r15d, 0xa0(%rbx,%r14)
 3c51e3b: 83 f9 03                     	cmpl	$0x3, %ecx
 3c51e3e: 77 0d                        	ja	0x3c51e4d <module_init+0x3c51e2d>
 3c51e40: 48 63 c1                     	movslq	%ecx, %rax
 3c51e43: 48 8d 15 f6 e6 1b 01         	leaq	0x11be6f6(%rip), %rdx   # 0x4e10540 <plt_log10+0x43fff0>
 3c51e4a: 8b 04 82                     	movl	(%rdx,%rax,4), %eax
 3c51e4d: 42 89 84 33 a4 00 00 00      	movl	%eax, 0xa4(%rbx,%r14)
 3c51e55: 48 8d 93 b8 0d 00 00         	leaq	0xdb8(%rbx), %rdx
 3c51e5c: 4c 89 ad 10 ff ff ff         	movq	%r13, -0xf0(%rbp)
 3c51e63: 48 89 bd 20 ff ff ff         	movq	%rdi, -0xe0(%rbp)
 3c51e6a: 48 8b b3 c0 0d 00 00         	movq	0xdc0(%rbx), %rsi
 3c51e71: 48 8b 8b 80 0d 00 00         	movq	0xd80(%rbx), %rcx
 3c51e78: 48 85 f6                     	testq	%rsi, %rsi
 3c51e7b: 48 0f 45 d6                  	cmovneq	%rsi, %rdx
 3c51e7f: 8b b3 c8 0d 00 00            	movl	0xdc8(%rbx), %esi
 3c51e85: 83 c6 03                     	addl	$0x3, %esi
 3c51e88: 21 c6                        	andl	%eax, %esi
 3c51e8a: 48 63 14 b2                  	movslq	(%rdx,%rsi,4), %rdx
 3c51e8e: 48 89 d6                     	movq	%rdx, %rsi
 3c51e91: 48 c1 e6 04                  	shlq	$0x4, %rsi
 3c51e95: 39 04 31                     	cmpl	%eax, (%rcx,%rsi)
 3c51e98: 74 1b                        	je	0x3c51eb5 <module_init+0x3c51e95>
 3c51e9a: 66 0f 1f 44 00 00            	nopw	(%rax,%rax)
 3c51ea0: 48 c1 e2 04                  	shlq	$0x4, %rdx
 3c51ea4: 48 63 54 11 08               	movslq	0x8(%rcx,%rdx), %rdx
 3c51ea9: 48 89 d6                     	movq	%rdx, %rsi
 3c51eac: 48 c1 e6 04                  	shlq	$0x4, %rsi
 3c51eb0: 39 04 31                     	cmpl	%eax, (%rcx,%rsi)
 3c51eb3: 75 eb                        	jne	0x3c51ea0 <module_init+0x3c51e80>
 3c51eb5: 48 c1 e2 04                  	shlq	$0x4, %rdx
 3c51eb9: 4c 8d 63 38                  	leaq	0x38(%rbx), %r12
 3c51ebd: 44 89 c6                     	movl	%r8d, %esi
 3c51ec0: 45 89 c5                     	movl	%r8d, %r13d
 3c51ec3: ff 44 11 04                  	incl	0x4(%rcx,%rdx)
 3c51ec7: 4c 89 e7                     	movq	%r12, %rdi
 3c51eca: 44 89 fa                     	movl	%r15d, %edx
 3c51ecd: e8 7e e0 ff ff               	callq	0x3c4ff50 <module_init+0x3c4ff30>
 3c51ed2: 84 c0                        	testb	%al, %al
 3c51ed4: 74 1e                        	je	0x3c51ef4 <module_init+0x3c51ed4>
 3c51ed6: 4a 8d b4 33 90 00 00 00      	leaq	0x90(%rbx,%r14), %rsi
 3c51ede: 48 89 df                     	movq	%rbx, %rdi
 3c51ee1: e8 6a 00 00 00               	callq	0x3c51f50 <module_init+0x3c51f30>
 3c51ee6: 0f b6 c8                     	movzbl	%al, %ecx
 3c51ee9: 4c 89 e7                     	movq	%r12, %rdi
 3c51eec: 44 89 ee                     	movl	%r13d, %esi
 3c51eef: 44 89 fa                     	movl	%r15d, %edx
 3c51ef2: eb 0b                        	jmp	0x3c51eff <module_init+0x3c51edf>
 3c51ef4: 4c 89 e7                     	movq	%r12, %rdi
 3c51ef7: 44 89 ee                     	movl	%r13d, %esi
 3c51efa: 44 89 fa                     	movl	%r15d, %edx
 3c51efd: 31 c9                        	xorl	%ecx, %ecx
 3c51eff: e8 3c e1 ff ff               	callq	0x3c50040 <module_init+0x3c50020>
 3c51f04: 4c 8b 25 3d 11 92 02         	movq	0x292113d(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
 3c51f0b: 4c 8b ad 10 ff ff ff         	movq	-0xf0(%rbp), %r13
 3c51f12: 4c 89 ef                     	movq	%r13, %rdi
 3c51f15: 48 c7 83 a0 11 00 00 00 00 00 00     	movq	$0x0, 0x11a0(%rbx)
 3c51f20: e8 8b 95 d7 00               	callq	0x49cb4b0 <plt_scePthreadMutexUnlock>
 3c51f25: 49 8b 04 24                  	movq	(%r12), %rax
 3c51f29: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
 3c51f2d: 75 19                        	jne	0x3c51f48 <module_init+0x3c51f28>
 3c51f2f: 48 8b 85 20 ff ff ff         	movq	-0xe0(%rbp), %rax
 3c51f36: 48 81 c4 c8 00 00 00         	addq	$0xc8, %rsp
 3c51f3d: 5b                           	popq	%rbx
 3c51f3e: 41 5c                        	popq	%r12
 3c51f40: 41 5d                        	popq	%r13
 3c51f42: 41 5e                        	popq	%r14
 3c51f44: 41 5f                        	popq	%r15
 3c51f46: 5d                           	popq	%rbp
 3c51f47: c3                           	retq
 3c51f48: e8 63 94 d7 00               	callq	0x49cb3b0 <plt___stack_chk_fail>
 3c51f4d: 0f 0b                        	ud2
 3c51f4f: 90                           	nop
 3c51f50: 55                           	pushq	%rbp
 3c51f51: 48 89 e5                     	movq	%rsp, %rbp
 3c51f54: 41 57                        	pushq	%r15
 3c51f56: 41 56                        	pushq	%r14
 3c51f58: 41 54                        	pushq	%r12
 3c51f5a: 53                           	pushq	%rbx
 3c51f5b: 4c 8d b7 98 11 00 00         	leaq	0x1198(%rdi), %r14
 3c51f62: 48 89 fb                     	movq	%rdi, %rbx
 3c51f65: 49 89 f4                     	movq	%rsi, %r12
 3c51f68: 4c 89 f7                     	movq	%r14, %rdi
 3c51f6b: e8 30 95 d7 00               	callq	0x49cb4a0 <plt_scePthreadMutexLock>
