
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 17da141: 4c 89 f7                     	movq	%r14, %rdi
 17da144: e8 57 13 1f 03               	callq	0x49cb4a0 <plt_scePthreadMutexLock>
 17da149: 85 c0                        	testl	%eax, %eax
 17da14b: 75 05                        	jne	0x17da152 <module_init+0x17da132>
 17da14d: e8 6e 13 1f 03               	callq	0x49cb4c0 <plt_scePthreadSelf>
 17da152: 48 8b 45 c8                  	movq	-0x38(%rbp), %rax
 17da156: 48 8b 4d a0                  	movq	-0x60(%rbp), %rcx
 17da15a: 4c 89 f7                     	movq	%r14, %rdi
 17da15d: 48 89 41 10                  	movq	%rax, 0x10(%rcx)
 17da161: c5 f8 10 45 b8               	vmovups	-0x48(%rbp), %xmm0
 17da166: c5 f8 11 01                  	vmovups	%xmm0, (%rcx)
 17da16a: c5 fb 10 45 a8               	vmovsd	-0x58(%rbp), %xmm0      # xmm0 = mem[0],zero
 17da16f: c5 fb 58 45 90               	vaddsd	-0x70(%rbp), %xmm0, %xmm0
 17da174: c5 fb 12 c0                  	vmovddup	%xmm0, %xmm0            # xmm0 = xmm0[0,0]
 17da178: c4 c1 79 58 45 28            	vaddpd	0x28(%r13), %xmm0, %xmm0
 17da17e: c4 c1 79 11 45 28            	vmovupd	%xmm0, 0x28(%r13)
 17da184: 49 ff 45 20                  	incq	0x20(%r13)
 17da188: 49 c7 45 10 00 00 00 00      	movq	$0x0, 0x10(%r13)
 17da190: e8 1b 13 1f 03               	callq	0x49cb4b0 <plt_scePthreadMutexUnlock>
 17da195: 48 8b 3d 84 10 2b 05         	movq	0x52b1084(%rip), %rdi   # 0x6a8b220
 17da19c: 48 8d 1d 4d d8 25 05         	leaq	0x525d84d(%rip), %rbx   # 0x6a379f0
 17da1a3: 4c 8d 25 36 d8 25 05         	leaq	0x525d836(%rip), %r12   # 0x6a379e0
 17da1aa: 48 85 ff                     	testq	%rdi, %rdi
 17da1ad: 0f 84 8d fe ff ff            	je	0x17da040 <module_init+0x17da020>
 17da1b3: 48 8b 07                     	movq	(%rdi), %rax
 17da1b6: ff 50 10                     	callq	*0x10(%rax)
 17da1b9: e9 82 fe ff ff               	jmp	0x17da040 <module_init+0x17da020>
