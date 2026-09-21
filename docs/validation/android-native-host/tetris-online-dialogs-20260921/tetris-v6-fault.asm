
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 103c1d0: 08 48 85                     	orb	%cl, -0x7b(%rax)
 103c1d3: c0 74 7b 48                  	<unknown>
 103c1d7: 89 ea                        	movl	%ebp, %edx
 103c1d9: 31 c9                        	xorl	%ecx, %ecx
 103c1db: 0f 1f 44 00 00               	nopl	(%rax,%rax)
 103c1e0: 48 89 84 cd 90 f0 ff ff      	movq	%rax, -0xf70(%rbp,%rcx,8)
 103c1e8: 48 83 f9 62                  	cmpq	$0x62, %rcx
 103c1ec: 48 8d 49 01                  	leaq	0x1(%rcx), %rcx
 103c1f0: 77 4c                        	ja	0x103c23e <module_init+0x103c21e>
 103c1f2: 48 8b 12                     	movq	(%rdx), %rdx
 103c1f5: 48 8b 42 08                  	movq	0x8(%rdx), %rax
 103c1f9: 48 85 c0                     	testq	%rax, %rax
 103c1fc: 75 e2                        	jne	0x103c1e0 <module_init+0x103c1c0>
 103c1fe: eb 3e                        	jmp	0x103c23e <module_init+0x103c21e>
 103c200: 48 8b 45 08                  	movq	0x8(%rbp), %rax
 103c204: 41 83 c5 02                  	addl	$0x2, %r13d
 103c208: 48 85 c0                     	testq	%rax, %rax
 103c20b: 74 41                        	je	0x103c24e <module_init+0x103c22e>
 103c20d: 48 89 ea                     	movq	%rbp, %rdx
 103c210: 31 c9                        	xorl	%ecx, %ecx
 103c212: 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00    	nopw	%cs:(%rax,%rax)
 103c220: 48 89 84 cd 90 f0 ff ff      	movq	%rax, -0xf70(%rbp,%rcx,8)
 103c228: 48 83 f9 62                  	cmpq	$0x62, %rcx
 103c22c: 48 8d 49 01                  	leaq	0x1(%rcx), %rcx
 103c230: 77 0c                        	ja	0x103c23e <module_init+0x103c21e>
 103c232: 48 8b 12                     	movq	(%rdx), %rdx
 103c235: 48 8b 42 08                  	movq	0x8(%rdx), %rax
 103c239: 48 85 c0                     	testq	%rax, %rax
 103c23c: 75 e2                        	jne	0x103c220 <module_init+0x103c200>
 103c23e: 41 bf 64 00 00 00            	movl	$0x64, %r15d
 103c244: 83 f9 63                     	cmpl	$0x63, %ecx
 103c247: 77 2a                        	ja	0x103c273 <module_init+0x103c253>
 103c249: 41 89 cf                     	movl	%ecx, %r15d
 103c24c: eb 03                        	jmp	0x103c251 <module_init+0x103c231>
 103c24e: 45 31 ff                     	xorl	%r15d, %r15d
 103c251: 44 89 f8                     	movl	%r15d, %eax
 103c254: 31 f6                        	xorl	%esi, %esi
 103c256: 48 8d bc c5 90 f0 ff ff      	leaq	-0xf70(%rbp,%rax,8), %rdi
 103c25e: b8 63 00 00 00               	movl	$0x63, %eax
 103c263: 44 29 f8                     	subl	%r15d, %eax
 103c266: 48 8d 14 c5 08 00 00 00      	leaq	0x8(,%rax,8), %rdx
 103c26e: e8 ad f1 98 03               	callq	0x49cb420 <plt_memset>
 103c273: 45 39 fd                     	cmpl	%r15d, %r13d
 103c276: 0f 83 fa 00 00 00            	jae	0x103c376 <module_init+0x103c356>
 103c27c: 48 83 bd 88 f0 ff ff 00      	cmpq	$0x0, -0xf78(%rbp)
 103c284: 0f 84 a2 00 00 00            	je	0x103c32c <module_init+0x103c30c>
 103c28a: 4d 85 e4                     	testq	%r12, %r12
 103c28d: 0f 84 99 00 00 00            	je	0x103c32c <module_init+0x103c30c>
 103c293: 44 89 e8                     	movl	%r13d, %eax
 103c296: 45 29 ef                     	subl	%r13d, %r15d
 103c299: 4c 89 a5 80 f0 ff ff         	movq	%r12, -0xf80(%rbp)
 103c2a0: 4c 8d b4 c5 90 f0 ff ff      	leaq	-0xf70(%rbp,%rax,8), %r14
 103c2a8: eb 1a                        	jmp	0x103c2c4 <module_init+0x103c2a4>
 103c2aa: 66 0f 1f 44 00 00            	nopw	(%rax,%rax)
