
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  5c3d61: 8b 7d cc                     	movl	-0x34(%rbp), %edi
  5c3d64: 8b 75 c8                     	movl	-0x38(%rbp), %esi
  5c3d67: e8 84 99 40 04               	callq	0x49cd6f0 <plt_sceNpScoreCreateNpTitleCtxA>
  5c3d6c: 89 43 40                     	movl	%eax, 0x40(%rbx)
  5c3d6f: 49 8b 07                     	movq	(%r15), %rax
  5c3d72: 48 3b 45 e0                  	cmpq	-0x20(%rbp), %rax
  5c3d76: 75 0b                        	jne	0x5c3d83 <module_init+0x5c3d63>
  5c3d78: 48 83 c4 28                  	addq	$0x28, %rsp
  5c3d7c: 5b                           	popq	%rbx
  5c3d7d: 41 5e                        	popq	%r14
  5c3d7f: 41 5f                        	popq	%r15
  5c3d81: 5d                           	popq	%rbp
  5c3d82: c3                           	retq
  5c3d83: e8 28 76 40 04               	callq	0x49cb3b0 <plt___stack_chk_fail>
  5c3d88: 0f 0b                        	ud2
  5c3d8a: 90                           	nop
  5c3d8b: 90                           	nop
  5c3d8c: 90                           	nop
  5c3d8d: 90                           	nop
  5c3d8e: 90                           	nop
  5c3d8f: 90                           	nop
  5c3d90: 55                           	pushq	%rbp
  5c3d91: 48 89 e5                     	movq	%rsp, %rbp
  5c3d94: 53                           	pushq	%rbx
  5c3d95: 50                           	pushq	%rax
  5c3d96: 48 8d 05 93 b7 8a 05         	leaq	0x58ab793(%rip), %rax   # 0x5e6f530 <plt_log10+0x149efe0>
  5c3d9d: 48 89 fb                     	movq	%rdi, %rbx
  5c3da0: 48 89 07                     	movq	%rax, (%rdi)
  5c3da3: 8b 7f 40                     	movl	0x40(%rdi), %edi
  5c3da6: e8 65 99 40 04               	callq	0x49cd710 <plt_sceNpScoreDeleteNpTitleCtx>
  5c3dab: bf 1e 00 00 00               	movl	$0x1e, %edi
  5c3db0: e8 db 9b 40 04               	callq	0x49cd990 <plt_sceSysmoduleUnloadModule>
  5c3db5: 48 8d 05 14 1a 88 05         	leaq	0x5881a14(%rip), %rax   # 0x5e457d0 <plt_log10+0x1475280>
  5c3dbc: 48 8d 7b 20                  	leaq	0x20(%rbx), %rdi
  5c3dc0: 48 89 03                     	movq	%rax, (%rbx)
  5c3dc3: e8 38 bd a4 ff               	callq	0xfb00 <module_init+0xfae0>
  5c3dc8: 48 83 c3 08                  	addq	$0x8, %rbx
  5c3dcc: 48 89 df                     	movq	%rbx, %rdi
  5c3dcf: 48 83 c4 08                  	addq	$0x8, %rsp
  5c3dd3: 5b                           	popq	%rbx
  5c3dd4: 5d                           	popq	%rbp
  5c3dd5: e9 26 bd a4 ff               	jmp	0xfb00 <module_init+0xfae0>
  5c3dda: 90                           	nop
  5c3ddb: 90                           	nop
  5c3ddc: 90                           	nop
  5c3ddd: 90                           	nop
  5c3dde: 90                           	nop
  5c3ddf: 90                           	nop
  5c3de0: 55                           	pushq	%rbp
  5c3de1: 48 89 e5                     	movq	%rsp, %rbp
  5c3de4: 53                           	pushq	%rbx
  5c3de5: 50                           	pushq	%rax
  5c3de6: 48 8d 05 43 b7 8a 05         	leaq	0x58ab743(%rip), %rax   # 0x5e6f530 <plt_log10+0x149efe0>
  5c3ded: 48 89 fb                     	movq	%rdi, %rbx
  5c3df0: 48 89 07                     	movq	%rax, (%rdi)
  5c3df3: 8b 7f 40                     	movl	0x40(%rdi), %edi
  5c3df6: e8 15 99 40 04               	callq	0x49cd710 <plt_sceNpScoreDeleteNpTitleCtx>
  5c3dfb: bf 1e 00 00 00               	movl	$0x1e, %edi
  5c3e00: e8 8b 9b 40 04               	callq	0x49cd990 <plt_sceSysmoduleUnloadModule>
  5c3e05: 48 8d 05 c4 19 88 05         	leaq	0x58819c4(%rip), %rax   # 0x5e457d0 <plt_log10+0x1475280>
  5c3e0c: 48 8d 7b 20                  	leaq	0x20(%rbx), %rdi
  5c3e10: 48 89 03                     	movq	%rax, (%rbx)
  5c3e13: e8 e8 bc a4 ff               	callq	0xfb00 <module_init+0xfae0>
  5c3e18: 48 8d 7b 08                  	leaq	0x8(%rbx), %rdi
  5c3e1c: e8 df bc a4 ff               	callq	0xfb00 <module_init+0xfae0>
  5c3e21: 48 89 df                     	movq	%rbx, %rdi
  5c3e24: 48 83 c4 08                  	addq	$0x8, %rsp
  5c3e28: 5b                           	popq	%rbx
  5c3e29: 5d                           	popq	%rbp
  5c3e2a: e9 31 d7 91 00               	jmp	0xee1560 <module_init+0xee1540>
  5c3e2f: 90                           	nop
  5c3e30: 55                           	pushq	%rbp
  5c3e31: 48 89 e5                     	movq	%rsp, %rbp
  5c3e34: 41 57                        	pushq	%r15
  5c3e36: 41 56                        	pushq	%r14
  5c3e38: 41 55                        	pushq	%r13
  5c3e3a: 41 54                        	pushq	%r12
  5c3e3c: 53                           	pushq	%rbx
  5c3e3d: 48 81 ec b8 00 00 00         	subq	$0xb8, %rsp
  5c3e44: 48 8b 1d fd f1 fa 05         	movq	0x5faf1fd(%rip), %rbx   # 0x6573048 <plt_log10+0x1ba2af8>
  5c3e4b: 49 89 fe                     	movq	%rdi, %r14
  5c3e4e: 48 8d 7d bc                  	leaq	-0x44(%rbp), %rdi
  5c3e52: 49 89 d4                     	movq	%rdx, %r12
  5c3e55: 49 89 f5                     	movq	%rsi, %r13
  5c3e58: 48 8b 03                     	movq	(%rbx), %rax
  5c3e5b: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
  5c3e5f: e8 7c 8e 40 04               	callq	0x49ccce0 <plt_sceUserServiceGetInitialUser>
  5c3e64: 41 89 c7                     	movl	%eax, %r15d
  5c3e67: 85 c0                        	testl	%eax, %eax
  5c3e69: 74 25                        	je	0x5c3e90 <module_init+0x5c3e70>
  5c3e6b: 45 85 ff                     	testl	%r15d, %r15d
  5c3e6e: 48 8b 0b                     	movq	(%rbx), %rcx
  5c3e71: 0f 94 c0                     	sete	%al
  5c3e74: 48 3b 4d d0                  	cmpq	-0x30(%rbp), %rcx
  5c3e78: 0f 85 33 04 00 00            	jne	0x5c42b1 <module_init+0x5c4291>
  5c3e7e: 48 81 c4 b8 00 00 00         	addq	$0xb8, %rsp
  5c3e85: 5b                           	popq	%rbx
  5c3e86: 41 5c                        	popq	%r12
  5c3e88: 41 5d                        	popq	%r13
  5c3e8a: 41 5e                        	popq	%r14
  5c3e8c: 41 5f                        	popq	%r15
  5c3e8e: 5d                           	popq	%rbp
  5c3e8f: c3                           	retq
  5c3e90: 48 8d 05 09 6f 4a 06         	leaq	0x64a6f09(%rip), %rax   # 0x6a6ada0
  5c3e97: 8b 75 bc                     	movl	-0x44(%rbp), %esi
  5c3e9a: 4c 89 a5 30 ff ff ff         	movq	%r12, -0xd0(%rbp)
  5c3ea1: 48 8b 38                     	movq	(%rax), %rdi
  5c3ea4: e8 a7 6e ea 00               	callq	0x146ad50 <module_init+0x146ad30>
  5c3ea9: 49 8b 76 38                  	movq	0x38(%r14), %rsi
  5c3ead: 89 c3                        	movl	%eax, %ebx
  5c3eaf: 48 8d 7d 80                  	leaq	-0x80(%rbp), %rdi
  5c3eb3: 48 8b 06                     	movq	(%rsi), %rax
  5c3eb6: ff 90 88 00 00 00            	callq	*0x88(%rax)
  5c3ebc: 48 8b 75 80                  	movq	-0x80(%rbp), %rsi
  5c3ec0: 48 8d bd 50 ff ff ff         	leaq	-0xb0(%rbp), %rdi
  5c3ec7: 89 da                        	movl	%ebx, %edx
  5c3ec9: 48 8b 06                     	movq	(%rsi), %rax
  5c3ecc: ff 90 f8 00 00 00            	callq	*0xf8(%rax)
  5c3ed2: 4c 8b a5 58 ff ff ff         	movq	-0xa8(%rbp), %r12
  5c3ed9: 48 8b 85 50 ff ff ff         	movq	-0xb0(%rbp), %rax
  5c3ee0: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5c3ee4: c5 f8 11 85 50 ff ff ff      	vmovups	%xmm0, -0xb0(%rbp)
  5c3eec: 4d 85 e4                     	testq	%r12, %r12
  5c3eef: 48 89 85 28 ff ff ff         	movq	%rax, -0xd8(%rbp)
  5c3ef6: 74 22                        	je	0x5c3f1a <module_init+0x5c3efa>
  5c3ef8: 41 83 7c 24 08 00            	cmpl	$0x0, 0x8(%r12)
  5c3efe: 75 1a                        	jne	0x5c3f1a <module_init+0x5c3efa>
  5c3f00: 49 8b 04 24                  	movq	(%r12), %rax
  5c3f04: 4c 89 e7                     	movq	%r12, %rdi
  5c3f07: ff 10                        	callq	*(%rax)
  5c3f09: 41 ff 4c 24 0c               	decl	0xc(%r12)
  5c3f0e: 75 0a                        	jne	0x5c3f1a <module_init+0x5c3efa>
  5c3f10: 49 8b 04 24                  	movq	(%r12), %rax
  5c3f14: 4c 89 e7                     	movq	%r12, %rdi
  5c3f17: ff 50 10                     	callq	*0x10(%rax)
  5c3f1a: 48 8b 9d 58 ff ff ff         	movq	-0xa8(%rbp), %rbx
