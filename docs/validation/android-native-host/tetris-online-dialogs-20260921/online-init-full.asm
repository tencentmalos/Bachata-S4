
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  5d8c00: c3                           	retq
  5d8c01: 90                           	nop
  5d8c02: 90                           	nop
  5d8c03: 90                           	nop
  5d8c04: 90                           	nop
  5d8c05: 90                           	nop
  5d8c06: 90                           	nop
  5d8c07: 90                           	nop
  5d8c08: 90                           	nop
  5d8c09: 90                           	nop
  5d8c0a: 90                           	nop
  5d8c0b: 90                           	nop
  5d8c0c: 90                           	nop
  5d8c0d: 90                           	nop
  5d8c0e: 90                           	nop
  5d8c0f: 90                           	nop
  5d8c10: 55                           	pushq	%rbp
  5d8c11: 48 89 e5                     	movq	%rsp, %rbp
  5d8c14: 48 8b 8e 10 03 00 00         	movq	0x310(%rsi), %rcx
  5d8c1b: 48 89 f8                     	movq	%rdi, %rax
  5d8c1e: 48 89 0f                     	movq	%rcx, (%rdi)
  5d8c21: 48 8b 8e 18 03 00 00         	movq	0x318(%rsi), %rcx
  5d8c28: 48 85 c9                     	testq	%rcx, %rcx
  5d8c2b: 48 89 4f 08                  	movq	%rcx, 0x8(%rdi)
  5d8c2f: 74 04                        	je	0x5d8c35 <module_init+0x5d8c15>
  5d8c31: f0                           	lock
  5d8c32: ff 41 08                     	incl	0x8(%rcx)
  5d8c35: 5d                           	popq	%rbp
  5d8c36: c3                           	retq
  5d8c37: 90                           	nop
  5d8c38: 90                           	nop
  5d8c39: 90                           	nop
  5d8c3a: 90                           	nop
  5d8c3b: 90                           	nop
  5d8c3c: 90                           	nop
  5d8c3d: 90                           	nop
  5d8c3e: 90                           	nop
  5d8c3f: 90                           	nop
  5d8c40: 55                           	pushq	%rbp
  5d8c41: 48 89 e5                     	movq	%rsp, %rbp
  5d8c44: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d8c48: 48 89 f8                     	movq	%rdi, %rax
  5d8c4b: c5 f8 11 07                  	vmovups	%xmm0, (%rdi)
  5d8c4f: 5d                           	popq	%rbp
  5d8c50: c3                           	retq
  5d8c51: 90                           	nop
  5d8c52: 90                           	nop
  5d8c53: 90                           	nop
  5d8c54: 90                           	nop
  5d8c55: 90                           	nop
  5d8c56: 90                           	nop
  5d8c57: 90                           	nop
  5d8c58: 90                           	nop
  5d8c59: 90                           	nop
  5d8c5a: 90                           	nop
  5d8c5b: 90                           	nop
  5d8c5c: 90                           	nop
  5d8c5d: 90                           	nop
  5d8c5e: 90                           	nop
  5d8c5f: 90                           	nop
  5d8c60: 55                           	pushq	%rbp
  5d8c61: 48 89 e5                     	movq	%rsp, %rbp
  5d8c64: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d8c68: 48 89 f8                     	movq	%rdi, %rax
  5d8c6b: c5 f8 11 07                  	vmovups	%xmm0, (%rdi)
  5d8c6f: 5d                           	popq	%rbp
  5d8c70: c3                           	retq
  5d8c71: 90                           	nop
  5d8c72: 90                           	nop
  5d8c73: 90                           	nop
  5d8c74: 90                           	nop
  5d8c75: 90                           	nop
  5d8c76: 90                           	nop
  5d8c77: 90                           	nop
  5d8c78: 90                           	nop
  5d8c79: 90                           	nop
  5d8c7a: 90                           	nop
  5d8c7b: 90                           	nop
  5d8c7c: 90                           	nop
  5d8c7d: 90                           	nop
  5d8c7e: 90                           	nop
  5d8c7f: 90                           	nop
  5d8c80: 55                           	pushq	%rbp
  5d8c81: 48 89 e5                     	movq	%rsp, %rbp
  5d8c84: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d8c88: 48 89 f8                     	movq	%rdi, %rax
  5d8c8b: c5 f8 11 07                  	vmovups	%xmm0, (%rdi)
  5d8c8f: 5d                           	popq	%rbp
  5d8c90: c3                           	retq
  5d8c91: 90                           	nop
  5d8c92: 90                           	nop
  5d8c93: 90                           	nop
  5d8c94: 90                           	nop
  5d8c95: 90                           	nop
  5d8c96: 90                           	nop
  5d8c97: 90                           	nop
  5d8c98: 90                           	nop
  5d8c99: 90                           	nop
  5d8c9a: 90                           	nop
  5d8c9b: 90                           	nop
  5d8c9c: 90                           	nop
  5d8c9d: 90                           	nop
  5d8c9e: 90                           	nop
  5d8c9f: 90                           	nop
  5d8ca0: 55                           	pushq	%rbp
  5d8ca1: 48 89 e5                     	movq	%rsp, %rbp
  5d8ca4: 48 8b 8e 30 03 00 00         	movq	0x330(%rsi), %rcx
  5d8cab: 48 89 f8                     	movq	%rdi, %rax
  5d8cae: 48 89 0f                     	movq	%rcx, (%rdi)
  5d8cb1: 48 8b 8e 38 03 00 00         	movq	0x338(%rsi), %rcx
  5d8cb8: 48 85 c9                     	testq	%rcx, %rcx
  5d8cbb: 48 89 4f 08                  	movq	%rcx, 0x8(%rdi)
  5d8cbf: 74 04                        	je	0x5d8cc5 <module_init+0x5d8ca5>
  5d8cc1: f0                           	lock
  5d8cc2: ff 41 08                     	incl	0x8(%rcx)
  5d8cc5: 5d                           	popq	%rbp
  5d8cc6: c3                           	retq
  5d8cc7: 90                           	nop
  5d8cc8: 90                           	nop
  5d8cc9: 90                           	nop
  5d8cca: 90                           	nop
  5d8ccb: 90                           	nop
  5d8ccc: 90                           	nop
  5d8ccd: 90                           	nop
  5d8cce: 90                           	nop
  5d8ccf: 90                           	nop
  5d8cd0: 55                           	pushq	%rbp
  5d8cd1: 48 89 e5                     	movq	%rsp, %rbp
  5d8cd4: 41 57                        	pushq	%r15
  5d8cd6: 41 56                        	pushq	%r14
  5d8cd8: 41 55                        	pushq	%r13
  5d8cda: 41 54                        	pushq	%r12
  5d8cdc: 53                           	pushq	%rbx
  5d8cdd: 48 81 ec 88 00 00 00         	subq	$0x88, %rsp
  5d8ce4: 4c 8b 25 5d a3 f9 05         	movq	0x5f9a35d(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d8ceb: 49 89 fe                     	movq	%rdi, %r14
  5d8cee: 49 8b 04 24                  	movq	(%r12), %rax
  5d8cf2: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
  5d8cf6: c7 87 38 04 00 00 00 01 00 00	movl	$0x100, 0x438(%rdi)     # imm = 0x100
  5d8d00: e8 fb 7f bd 00               	callq	0x11b0d00 <module_init+0x11b0ce0>
  5d8d05: 48 8d 35 bc 40 44 04         	leaq	0x44440bc(%rip), %rsi   # 0x4a1cdc8 <plt_log10+0x4c878>
  5d8d0c: 48 8d 7d b8                  	leaq	-0x48(%rbp), %rdi
  5d8d10: ba 01 00 00 00               	movl	$0x1, %edx
  5d8d15: 48 89 c3                     	movq	%rax, %rbx
  5d8d18: e8 23 23 c2 00               	callq	0x11fb040 <module_init+0x11fb020>
  5d8d1d: 48 8b 75 b8                  	movq	-0x48(%rbp), %rsi
  5d8d21: 48 89 df                     	movq	%rbx, %rdi
  5d8d24: e8 47 8e bd 00               	callq	0x11b1b70 <module_init+0x11b1b50>
  5d8d29: 4c 8d 2d 60 6a 45 06         	leaq	0x6456a60(%rip), %r13   # 0x6a2f790
  5d8d30: c5 f9 76 c0                  	vpcmpeqd	%xmm0, %xmm0, %xmm0
  5d8d34: 48 8d 1d 4d eb 45 06         	leaq	0x645eb4d(%rip), %rbx   # 0x6a37888
  5d8d3b: 4c 8d 3d 90 40 44 04         	leaq	0x4444090(%rip), %r15   # 0x4a1cdd2 <plt_log10+0x4c882>
  5d8d42: 49 8d 8e 18 04 00 00         	leaq	0x418(%r14), %rcx
  5d8d49: 48 8d 15 a8 40 44 04         	leaq	0x44440a8(%rip), %rdx   # 0x4a1cdf8 <plt_log10+0x4c8a8>
  5d8d50: c4 c1 7a 7f 86 08 04 00 00   	vmovdqu	%xmm0, 0x408(%r14)
  5d8d59: 4c 89 fe                     	movq	%r15, %rsi
  5d8d5c: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8d60: 49 89 d8                     	movq	%rbx, %r8
  5d8d63: e8 58 29 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8d68: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8d6c: 49 8d 8e 19 04 00 00         	leaq	0x419(%r14), %rcx
  5d8d73: 48 8d 15 a0 40 44 04         	leaq	0x44440a0(%rip), %rdx   # 0x4a1ce1a <plt_log10+0x4c8ca>
  5d8d7a: 4c 89 fe                     	movq	%r15, %rsi
  5d8d7d: 49 89 d8                     	movq	%rbx, %r8
  5d8d80: 48 89 4d 88                  	movq	%rcx, -0x78(%rbp)
  5d8d84: e8 37 29 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8d89: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8d8d: 49 8d 8e 1a 04 00 00         	leaq	0x41a(%r14), %rcx
  5d8d94: 48 8d 15 a9 40 44 04         	leaq	0x44440a9(%rip), %rdx   # 0x4a1ce44 <plt_log10+0x4c8f4>
  5d8d9b: 4c 89 fe                     	movq	%r15, %rsi
  5d8d9e: 49 89 d8                     	movq	%rbx, %r8
  5d8da1: 48 89 8d 60 ff ff ff         	movq	%rcx, -0xa0(%rbp)
  5d8da8: e8 13 29 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8dad: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8db1: 49 8d 8e 1b 04 00 00         	leaq	0x41b(%r14), %rcx
  5d8db8: 48 8d 15 a9 40 44 04         	leaq	0x44440a9(%rip), %rdx   # 0x4a1ce68 <plt_log10+0x4c918>
  5d8dbf: 4c 89 fe                     	movq	%r15, %rsi
  5d8dc2: 49 89 d8                     	movq	%rbx, %r8
  5d8dc5: 48 89 8d 70 ff ff ff         	movq	%rcx, -0x90(%rbp)
  5d8dcc: e8 ef 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8dd1: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8dd5: 49 8d 8e 1c 04 00 00         	leaq	0x41c(%r14), %rcx
  5d8ddc: 48 8d 15 af 40 44 04         	leaq	0x44440af(%rip), %rdx   # 0x4a1ce92 <plt_log10+0x4c942>
  5d8de3: 4c 89 fe                     	movq	%r15, %rsi
  5d8de6: 49 89 d8                     	movq	%rbx, %r8
  5d8de9: 48 89 8d 58 ff ff ff         	movq	%rcx, -0xa8(%rbp)
  5d8df0: e8 cb 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8df5: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8df9: 49 8d 8e 1d 04 00 00         	leaq	0x41d(%r14), %rcx
  5d8e00: 48 8d 15 af 40 44 04         	leaq	0x44440af(%rip), %rdx   # 0x4a1ceb6 <plt_log10+0x4c966>
  5d8e07: 4c 89 fe                     	movq	%r15, %rsi
  5d8e0a: 49 89 d8                     	movq	%rbx, %r8
  5d8e0d: 48 89 8d 68 ff ff ff         	movq	%rcx, -0x98(%rbp)
  5d8e14: e8 a7 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8e19: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8e1d: 49 8d 8e 1e 04 00 00         	leaq	0x41e(%r14), %rcx
  5d8e24: 48 8d 15 ab 40 44 04         	leaq	0x44440ab(%rip), %rdx   # 0x4a1ced6 <plt_log10+0x4c986>
  5d8e2b: 4c 89 fe                     	movq	%r15, %rsi
  5d8e2e: 49 89 d8                     	movq	%rbx, %r8
  5d8e31: e8 8a 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8e36: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8e3a: 49 8d 8e 1f 04 00 00         	leaq	0x41f(%r14), %rcx
  5d8e41: 48 8d 15 b2 40 44 04         	leaq	0x44440b2(%rip), %rdx   # 0x4a1cefa <plt_log10+0x4c9aa>
  5d8e48: 4c 89 fe                     	movq	%r15, %rsi
  5d8e4b: 49 89 d8                     	movq	%rbx, %r8
  5d8e4e: 48 89 8d 50 ff ff ff         	movq	%rcx, -0xb0(%rbp)
  5d8e55: e8 66 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8e5a: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8e5e: 49 8d 8e 20 04 00 00         	leaq	0x420(%r14), %rcx
  5d8e65: 48 8d 15 b6 40 44 04         	leaq	0x44440b6(%rip), %rdx   # 0x4a1cf22 <plt_log10+0x4c9d2>
  5d8e6c: 4c 89 fe                     	movq	%r15, %rsi
  5d8e6f: 49 89 d8                     	movq	%rbx, %r8
  5d8e72: e8 49 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8e77: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8e7b: 49 8d 8e 21 04 00 00         	leaq	0x421(%r14), %rcx
  5d8e82: 48 8d 15 bb 40 44 04         	leaq	0x44440bb(%rip), %rdx   # 0x4a1cf44 <plt_log10+0x4c9f4>
  5d8e89: 4c 89 fe                     	movq	%r15, %rsi
  5d8e8c: 49 89 d8                     	movq	%rbx, %r8
  5d8e8f: 48 89 8d 78 ff ff ff         	movq	%rcx, -0x88(%rbp)
  5d8e96: e8 25 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8e9b: 49 8b 7d 00                  	movq	(%r13), %rdi
  5d8e9f: 4d 8d ae 22 04 00 00         	leaq	0x422(%r14), %r13
  5d8ea6: 48 8d 35 b3 40 44 04         	leaq	0x44440b3(%rip), %rsi   # 0x4a1cf60 <plt_log10+0x4ca10>
  5d8ead: 48 8d 15 cc 40 44 04         	leaq	0x44440cc(%rip), %rdx   # 0x4a1cf80 <plt_log10+0x4ca30>
  5d8eb4: 49 89 d8                     	movq	%rbx, %r8
  5d8eb7: 4c 89 e9                     	movq	%r13, %rcx
  5d8eba: e8 01 28 b6 00               	callq	0x113b6c0 <module_init+0x113b6a0>
  5d8ebf: 48 8b 35 6a bf 04 06         	movq	0x604bf6a(%rip), %rsi   # 0x6624e30
  5d8ec6: 48 85 f6                     	testq	%rsi, %rsi
  5d8ec9: 75 33                        	jne	0x5d8efe <module_init+0x5d8ede>
  5d8ecb: 48 8d 35 bc 66 44 04         	leaq	0x44466bc(%rip), %rsi   # 0x4a1f58e <plt_log10+0x4f03e>
  5d8ed2: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8ed6: ba 01 00 00 00               	movl	$0x1, %edx
  5d8edb: e8 60 21 c2 00               	callq	0x11fb040 <module_init+0x11fb020>
  5d8ee0: 48 8b 5d c0                  	movq	-0x40(%rbp), %rbx
  5d8ee4: e8 17 7e bd 00               	callq	0x11b0d00 <module_init+0x11b0ce0>
  5d8ee9: 48 89 c7                     	movq	%rax, %rdi
  5d8eec: 48 89 de                     	movq	%rbx, %rsi
  5d8eef: e8 fc 90 bd 00               	callq	0x11b1ff0 <module_init+0x11b1fd0>
  5d8ef4: 48 89 c6                     	movq	%rax, %rsi
  5d8ef7: 48 89 05 32 bf 04 06         	movq	%rax, 0x604bf32(%rip)   # 0x6624e30
  5d8efe: 48 8b 06                     	movq	(%rsi), %rax
  5d8f01: 48 8d 7d a8                  	leaq	-0x58(%rbp), %rdi
  5d8f05: ff 50 58                     	callq	*0x58(%rax)
  5d8f08: 48 8b 5d a8                  	movq	-0x58(%rbp), %rbx
  5d8f0c: 48 85 db                     	testq	%rbx, %rbx
  5d8f0f: 0f 84 c3 00 00 00            	je	0x5d8fd8 <module_init+0x5d8fb8>
  5d8f15: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8f19: be 13 00 00 00               	movl	$0x13, %esi
  5d8f1e: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d8f22: c5 fa 7f 45 c0               	vmovdqu	%xmm0, -0x40(%rbp)
  5d8f27: e8 e4 60 a3 ff               	callq	0xf010 <module_init+0xeff0>
  5d8f2c: 8b 75 c8                     	movl	-0x38(%rbp), %esi
  5d8f2f: 8d 46 13                     	leal	0x13(%rsi), %eax
  5d8f32: 89 45 c8                     	movl	%eax, -0x38(%rbp)
  5d8f35: 3b 45 cc                     	cmpl	-0x34(%rbp), %eax
  5d8f38: 7e 09                        	jle	0x5d8f43 <module_init+0x5d8f23>
  5d8f3a: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8f3e: e8 bd 65 a3 ff               	callq	0xf500 <module_init+0xf4e0>
  5d8f43: 48 8b 7d c0                  	movq	-0x40(%rbp), %rdi
  5d8f47: 48 8d 35 54 40 44 04         	leaq	0x4444054(%rip), %rsi   # 0x4a1cfa2 <plt_log10+0x4ca52>
  5d8f4e: ba 26 00 00 00               	movl	$0x26, %edx
  5d8f53: e8 68 38 a6 00               	callq	0x103c7c0 <module_init+0x103c7a0>
  5d8f58: 48 8b 55 88                  	movq	-0x78(%rbp), %rdx
  5d8f5c: 4c 8d 7d c0                  	leaq	-0x40(%rbp), %r15
  5d8f60: 48 89 df                     	movq	%rbx, %rdi
  5d8f63: 4c 89 fe                     	movq	%r15, %rsi
  5d8f66: e8 05 a4 c2 00               	callq	0x1203370 <module_init+0x1203350>
  5d8f6b: 4c 89 ff                     	movq	%r15, %rdi
  5d8f6e: e8 cd 5f a3 ff               	callq	0xef40 <module_init+0xef20>
  5d8f73: 4c 8b 7d a8                  	movq	-0x58(%rbp), %r15
  5d8f77: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8f7b: be 0a 00 00 00               	movl	$0xa, %esi
  5d8f80: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d8f84: c5 fa 7f 45 c0               	vmovdqu	%xmm0, -0x40(%rbp)
  5d8f89: e8 82 60 a3 ff               	callq	0xf010 <module_init+0xeff0>
  5d8f8e: 8b 75 c8                     	movl	-0x38(%rbp), %esi
  5d8f91: 8d 46 0a                     	leal	0xa(%rsi), %eax
  5d8f94: 89 45 c8                     	movl	%eax, -0x38(%rbp)
  5d8f97: 3b 45 cc                     	cmpl	-0x34(%rbp), %eax
  5d8f9a: 7e 09                        	jle	0x5d8fa5 <module_init+0x5d8f85>
  5d8f9c: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d8fa0: e8 5b 65 a3 ff               	callq	0xf500 <module_init+0xf4e0>
  5d8fa5: 48 8b 7d c0                  	movq	-0x40(%rbp), %rdi
  5d8fa9: 48 8d 35 18 40 44 04         	leaq	0x4444018(%rip), %rsi   # 0x4a1cfc8 <plt_log10+0x4ca78>
  5d8fb0: ba 14 00 00 00               	movl	$0x14, %edx
  5d8fb5: e8 06 38 a6 00               	callq	0x103c7c0 <module_init+0x103c7a0>
  5d8fba: 48 8b 95 78 ff ff ff         	movq	-0x88(%rbp), %rdx
  5d8fc1: 48 8d 5d c0                  	leaq	-0x40(%rbp), %rbx
  5d8fc5: 4c 89 ff                     	movq	%r15, %rdi
  5d8fc8: 48 89 de                     	movq	%rbx, %rsi
  5d8fcb: e8 a0 a3 c2 00               	callq	0x1203370 <module_init+0x1203350>
  5d8fd0: 48 89 df                     	movq	%rbx, %rdi
  5d8fd3: e8 68 5f a3 ff               	callq	0xef40 <module_init+0xef20>
  5d8fd8: e8 43 fe b5 00               	callq	0x1138e20 <module_init+0x1138e00>
  5d8fdd: 48 8d 35 f8 3f 44 04         	leaq	0x4443ff8(%rip), %rsi   # 0x4a1cfdc <plt_log10+0x4ca8c>
  5d8fe4: 48 89 c7                     	movq	%rax, %rdi
  5d8fe7: e8 b4 8f bb 00               	callq	0x1191fa0 <module_init+0x1191f80>
  5d8fec: 84 c0                        	testb	%al, %al
  5d8fee: 74 07                        	je	0x5d8ff7 <module_init+0x5d8fd7>
  5d8ff0: 48 8b 45 88                  	movq	-0x78(%rbp), %rax
  5d8ff4: c6 00 00                     	movb	$0x0, (%rax)
  5d8ff7: 4c 89 f7                     	movq	%r14, %rdi
  5d8ffa: e8 c1 d9 ff ff               	callq	0x5d69c0 <module_init+0x5d69a0>
  5d8fff: 84 c0                        	testb	%al, %al
  5d9001: 74 1b                        	je	0x5d901e <module_init+0x5d8ffe>
  5d9003: 48 8d 7d a0                  	leaq	-0x60(%rbp), %rdi
  5d9007: c7 45 a0 bc 02 00 00         	movl	$0x2bc, -0x60(%rbp)     # imm = 0x2BC
  5d900e: e8 dd 3c 3f 04               	callq	0x49cccf0 <plt_sceUserServiceInitialize>
  5d9013: 85 c0                        	testl	%eax, %eax
  5d9015: 79 58                        	jns	0x5d906f <module_init+0x5d904f>
  5d9017: 3d 03 00 96 80               	cmpl	$0x80960003, %eax       # imm = 0x80960003
  5d901c: 74 51                        	je	0x5d906f <module_init+0x5d904f>
  5d901e: 45 31 ed                     	xorl	%r13d, %r13d
  5d9021: 45 88 ae 24 04 00 00         	movb	%r13b, 0x424(%r14)
  5d9028: 48 8b 5d b0                  	movq	-0x50(%rbp), %rbx
  5d902c: 48 85 db                     	testq	%rbx, %rbx
  5d902f: 74 1b                        	je	0x5d904c <module_init+0x5d902c>
  5d9031: ff 4b 08                     	decl	0x8(%rbx)
  5d9034: 75 16                        	jne	0x5d904c <module_init+0x5d902c>
  5d9036: 48 8b 03                     	movq	(%rbx), %rax
  5d9039: 48 89 df                     	movq	%rbx, %rdi
  5d903c: ff 10                        	callq	*(%rax)
  5d903e: ff 4b 0c                     	decl	0xc(%rbx)
  5d9041: 75 09                        	jne	0x5d904c <module_init+0x5d902c>
  5d9043: 48 8b 03                     	movq	(%rbx), %rax
  5d9046: 48 89 df                     	movq	%rbx, %rdi
  5d9049: ff 50 10                     	callq	*0x10(%rax)
  5d904c: 49 8b 04 24                  	movq	(%r12), %rax
  5d9050: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
  5d9054: 0f 85 6c 19 00 00            	jne	0x5da9c6 <module_init+0x5da9a6>
  5d905a: 44 89 e8                     	movl	%r13d, %eax
  5d905d: 48 81 c4 88 00 00 00         	addq	$0x88, %rsp
  5d9064: 5b                           	popq	%rbx
  5d9065: 41 5c                        	popq	%r12
  5d9067: 41 5d                        	popq	%r13
  5d9069: 41 5e                        	popq	%r14
  5d906b: 41 5f                        	popq	%r15
  5d906d: 5d                           	popq	%rbp
  5d906e: c3                           	retq
  5d906f: bf c8 00 00 00               	movl	$0xc8, %edi
  5d9074: 4c 89 6d 80                  	movq	%r13, -0x80(%rbp)
  5d9078: 41 c6 86 23 04 00 00 01      	movb	$0x1, 0x423(%r14)
  5d9080: e8 1b 83 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9085: 48 89 c7                     	movq	%rax, %rdi
  5d9088: 48 89 c3                     	movq	%rax, %rbx
  5d908b: e8 90 67 f1 ff               	callq	0x4ef820 <module_init+0x4ef800>
  5d9090: 4c 8d 7d 90                  	leaq	-0x70(%rbp), %r15
  5d9094: 48 8d 05 c5 72 89 05         	leaq	0x58972c5(%rip), %rax   # 0x5e70360 <plt_log10+0x149fe10>
  5d909b: 48 8d 0d 16 73 89 05         	leaq	0x5897316(%rip), %rcx   # 0x5e703b8 <plt_log10+0x149fe68>
  5d90a2: 49 8d b6 c0 00 00 00         	leaq	0xc0(%r14), %rsi
  5d90a9: 4c 89 ff                     	movq	%r15, %rdi
  5d90ac: 48 89 03                     	movq	%rax, (%rbx)
  5d90af: 48 89 4b 08                  	movq	%rcx, 0x8(%rbx)
  5d90b3: 4c 89 b3 c0 00 00 00         	movq	%r14, 0xc0(%rbx)
  5d90ba: 49 89 9e 40 03 00 00         	movq	%rbx, 0x340(%r14)
  5d90c1: e8 2a 2f c2 00               	callq	0x11fbff0 <module_init+0x11fbfd0>
  5d90c6: 83 7d 98 00                  	cmpl	$0x0, -0x68(%rbp)
  5d90ca: 4c 8d 25 61 38 44 04         	leaq	0x4443861(%rip), %r12   # 0x4a1c932 <plt_log10+0x4c3e2>
  5d90d1: 4c 89 e2                     	movq	%r12, %rdx
  5d90d4: 74 04                        	je	0x5d90da <module_init+0x5d90ba>
  5d90d6: 48 8b 55 90                  	movq	-0x70(%rbp), %rdx
  5d90da: 4c 8d 6d c0                  	leaq	-0x40(%rbp), %r13
  5d90de: 48 8d 35 15 3f 44 04         	leaq	0x4443f15(%rip), %rsi   # 0x4a1cffa <plt_log10+0x4caaa>
  5d90e5: 31 c0                        	xorl	%eax, %eax
  5d90e7: 4c 89 ef                     	movq	%r13, %rdi
  5d90ea: e8 91 9f a3 00               	callq	0x1013080 <module_init+0x1013060>
  5d90ef: 83 7d c8 00                  	cmpl	$0x0, -0x38(%rbp)
  5d90f3: 74 04                        	je	0x5d90f9 <module_init+0x5d90d9>
  5d90f5: 4c 8b 65 c0                  	movq	-0x40(%rbp), %r12
  5d90f9: 41 b8 3f 00 00 00            	movl	$0x3f, %r8d
  5d90ff: 48 89 df                     	movq	%rbx, %rdi
  5d9102: 4c 89 e6                     	movq	%r12, %rsi
  5d9105: 31 d2                        	xorl	%edx, %edx
  5d9107: 31 c9                        	xorl	%ecx, %ecx
  5d9109: 45 31 c9                     	xorl	%r9d, %r9d
  5d910c: e8 5f c9 a8 00               	callq	0x1065a70 <module_init+0x1065a50>
  5d9111: 4c 89 ef                     	movq	%r13, %rdi
  5d9114: 49 89 86 48 03 00 00         	movq	%rax, 0x348(%r14)
  5d911b: e8 20 5e a3 ff               	callq	0xef40 <module_init+0xef20>
  5d9120: 4c 89 ff                     	movq	%r15, %rdi
  5d9123: e8 18 5e a3 ff               	callq	0xef40 <module_init+0xef20>
  5d9128: 4c 89 f7                     	movq	%r14, %rdi
  5d912b: e8 a0 ec ff ff               	callq	0x5d7dd0 <module_init+0x5d7db0>
  5d9130: 84 c0                        	testb	%al, %al
  5d9132: 0f 84 33 01 00 00            	je	0x5d926b <module_init+0x5d924b>
  5d9138: bf a8 00 00 00               	movl	$0xa8, %edi
  5d913d: e8 9e 23 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9142: 4c 8b 25 ff 9e f9 05         	movq	0x5f99eff(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d9149: 85 c0                        	testl	%eax, %eax
  5d914b: 0f 88 cd fe ff ff            	js	0x5d901e <module_init+0x5d8ffe>
  5d9151: bf e7 00 00 00               	movl	$0xe7, %edi
  5d9156: e8 85 23 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d915b: 85 c0                        	testl	%eax, %eax
  5d915d: 0f 88 bb fe ff ff            	js	0x5d901e <module_init+0x5d8ffe>
  5d9163: 48 8d 3d dc 73 44 04         	leaq	0x44473dc(%rip), %rdi   # 0x4a20546 <plt_log10+0x4fff6>
  5d916a: be 00 40 00 00               	movl	$0x4000, %esi           # imm = 0x4000
  5d916f: 31 d2                        	xorl	%edx, %edx
  5d9171: e8 4a 43 3f 04               	callq	0x49cd4c0 <plt_sceNetPoolCreate>
  5d9176: 85 c0                        	testl	%eax, %eax
  5d9178: 78 6c                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d917a: bf 00 00 04 00               	movl	$0x40000, %edi          # imm = 0x40000
  5d917f: 41 89 86 28 04 00 00         	movl	%eax, 0x428(%r14)
  5d9186: e8 d5 47 3f 04               	callq	0x49cd960 <plt_sceSslInit>
  5d918b: 85 c0                        	testl	%eax, %eax
  5d918d: 78 57                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d918f: 41 89 86 2c 04 00 00         	movl	%eax, 0x42c(%r14)
  5d9196: ba 00 00 04 00               	movl	$0x40000, %edx          # imm = 0x40000
  5d919b: 89 c6                        	movl	%eax, %esi
  5d919d: 41 8b be 28 04 00 00         	movl	0x428(%r14), %edi
  5d91a4: e8 57 42 3f 04               	callq	0x49cd400 <plt_sceHttpInit>
  5d91a9: 85 c0                        	testl	%eax, %eax
  5d91ab: 78 39                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d91ad: be 00 00 10 00               	movl	$0x100000, %esi         # imm = 0x100000
  5d91b2: 89 c7                        	movl	%eax, %edi
  5d91b4: 41 89 86 30 04 00 00         	movl	%eax, 0x430(%r14)
  5d91bb: e8 00 47 3f 04               	callq	0x49cd8c0 <plt_sceNpWebApiInitialize>
  5d91c0: 85 c0                        	testl	%eax, %eax
  5d91c2: 78 22                        	js	0x5d91e6 <module_init+0x5d91c6>
  5d91c4: bf 08 00 00 00               	movl	$0x8, %edi
  5d91c9: 41 89 86 34 04 00 00         	movl	%eax, 0x434(%r14)
  5d91d0: e8 cb 81 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d91d5: 48 8d 0d d4 82 89 05         	leaq	0x58982d4(%rip), %rcx   # 0x5e714b0 <plt_log10+0x14a0f60>
  5d91dc: 48 89 08                     	movq	%rcx, (%rax)
  5d91df: 49 89 86 08 01 00 00         	movq	%rax, 0x108(%r14)
  5d91e6: bf 00 04 00 00               	movl	$0x400, %edi            # imm = 0x400
  5d91eb: e8 b0 81 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d91f0: 48 89 c3                     	movq	%rax, %rbx
  5d91f3: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d91fd: 48 8d 0d ac af 89 05         	leaq	0x589afac(%rip), %rcx   # 0x5e741b0 <plt_log10+0x14a3c60>
  5d9204: 4c 89 f6                     	movq	%r14, %rsi
  5d9207: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d920b: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d920f: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9212: 4c 89 ff                     	movq	%r15, %rdi
  5d9215: e8 c6 5c 07 00               	callq	0x64eee0 <module_init+0x64eec0>
  5d921a: f0                           	lock
  5d921b: ff 43 08                     	incl	0x8(%rbx)
  5d921e: 4d 89 be 40 02 00 00         	movq	%r15, 0x240(%r14)
  5d9225: 4d 8b be 48 02 00 00         	movq	0x248(%r14), %r15
  5d922c: 49 39 df                     	cmpq	%rbx, %r15
  5d922f: 0f 84 9b 00 00 00            	je	0x5d92d0 <module_init+0x5d92b0>
  5d9235: 4d 85 ff                     	testq	%r15, %r15
  5d9238: 49 89 9e 48 02 00 00         	movq	%rbx, 0x248(%r14)
  5d923f: 0f 84 a8 00 00 00            	je	0x5d92ed <module_init+0x5d92cd>
  5d9245: f0                           	lock
  5d9246: 41 ff 4f 08                  	decl	0x8(%r15)
  5d924a: 0f 85 9d 00 00 00            	jne	0x5d92ed <module_init+0x5d92cd>
  5d9250: 49 8b 07                     	movq	(%r15), %rax
  5d9253: 4c 89 ff                     	movq	%r15, %rdi
  5d9256: ff 10                        	callq	*(%rax)
  5d9258: f0                           	lock
  5d9259: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d925d: 0f 85 8a 00 00 00            	jne	0x5d92ed <module_init+0x5d92cd>
  5d9263: 49 8b 07                     	movq	(%r15), %rax
  5d9266: 4c 89 ff                     	movq	%r15, %rdi
  5d9269: eb 7f                        	jmp	0x5d92ea <module_init+0x5d92ca>
  5d926b: 4c 8d 7d c0                  	leaq	-0x40(%rbp), %r15
  5d926f: 48 8d 35 bc 3d 44 04         	leaq	0x4443dbc(%rip), %rsi   # 0x4a1d032 <plt_log10+0x4cae2>
  5d9276: 45 31 ed                     	xorl	%r13d, %r13d
  5d9279: 31 c0                        	xorl	%eax, %eax
  5d927b: 4c 89 ff                     	movq	%r15, %rdi
  5d927e: e8 fd 9d a3 00               	callq	0x1013080 <module_init+0x1013060>
  5d9283: 83 7d c8 00                  	cmpl	$0x0, -0x38(%rbp)
  5d9287: 4c 8b 25 ba 9d f9 05         	movq	0x5f99dba(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d928e: 74 06                        	je	0x5d9296 <module_init+0x5d9276>
  5d9290: 4c 8b 45 c0                  	movq	-0x40(%rbp), %r8
  5d9294: eb 07                        	jmp	0x5d929d <module_init+0x5d927d>
  5d9296: 4c 8d 05 95 36 44 04         	leaq	0x4443695(%rip), %r8    # 0x4a1c932 <plt_log10+0x4c3e2>
  5d929d: 48 8d 3d 97 71 44 04         	leaq	0x4447197(%rip), %rdi   # 0x4a2043b <plt_log10+0x4feeb>
  5d92a4: 48 8d 15 f7 36 44 04         	leaq	0x44436f7(%rip), %rdx   # 0x4a1c9a2 <plt_log10+0x4c452>
  5d92ab: 48 8d 0d fa 36 44 04         	leaq	0x44436fa(%rip), %rcx   # 0x4a1c9ac <plt_log10+0x4c45c>
  5d92b2: be f3 02 00 00               	movl	$0x2f3, %esi            # imm = 0x2F3
  5d92b7: 31 c0                        	xorl	%eax, %eax
  5d92b9: e8 e2 20 b6 00               	callq	0x113b3a0 <module_init+0x113b380>
  5d92be: 4c 89 ff                     	movq	%r15, %rdi
  5d92c1: e8 7a 5c a3 ff               	callq	0xef40 <module_init+0xef20>
  5d92c6: e8 a5 20 b6 00               	callq	0x113b370 <module_init+0x113b350>
  5d92cb: e9 51 fd ff ff               	jmp	0x5d9021 <module_init+0x5d9001>
  5d92d0: f0                           	lock
  5d92d1: ff 4b 08                     	decl	0x8(%rbx)
  5d92d4: 75 17                        	jne	0x5d92ed <module_init+0x5d92cd>
  5d92d6: 48 8b 03                     	movq	(%rbx), %rax
  5d92d9: 48 89 df                     	movq	%rbx, %rdi
  5d92dc: ff 10                        	callq	*(%rax)
  5d92de: f0                           	lock
  5d92df: ff 4b 0c                     	decl	0xc(%rbx)
  5d92e2: 75 09                        	jne	0x5d92ed <module_init+0x5d92cd>
  5d92e4: 48 8b 03                     	movq	(%rbx), %rax
  5d92e7: 48 89 df                     	movq	%rbx, %rdi
  5d92ea: ff 50 10                     	callq	*0x10(%rax)
  5d92ed: f0                           	lock
  5d92ee: ff 4b 08                     	decl	0x8(%rbx)
  5d92f1: 75 17                        	jne	0x5d930a <module_init+0x5d92ea>
  5d92f3: 48 8b 03                     	movq	(%rbx), %rax
  5d92f6: 48 89 df                     	movq	%rbx, %rdi
  5d92f9: ff 10                        	callq	*(%rax)
  5d92fb: f0                           	lock
  5d92fc: ff 4b 0c                     	decl	0xc(%rbx)
  5d92ff: 75 09                        	jne	0x5d930a <module_init+0x5d92ea>
  5d9301: 48 8b 03                     	movq	(%rbx), %rax
  5d9304: 48 89 df                     	movq	%rbx, %rdi
  5d9307: ff 50 10                     	callq	*0x10(%rax)
  5d930a: 49 8b be 40 02 00 00         	movq	0x240(%r14), %rdi
  5d9311: e8 fa ea fe ff               	callq	0x5c7e10 <module_init+0x5c7df0>
  5d9316: bf d8 01 00 00               	movl	$0x1d8, %edi            # imm = 0x1D8
  5d931b: e8 80 80 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9320: 48 89 c3                     	movq	%rax, %rbx
  5d9323: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d932d: 48 8d 0d a4 ae 89 05         	leaq	0x589aea4(%rip), %rcx   # 0x5e741d8 <plt_log10+0x14a3c88>
  5d9334: 4c 89 f6                     	movq	%r14, %rsi
  5d9337: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d933b: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d933f: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9342: 4c 89 ff                     	movq	%r15, %rdi
  5d9345: e8 46 81 fe ff               	callq	0x5c1490 <module_init+0x5c1470>
  5d934a: f0                           	lock
  5d934b: ff 43 08                     	incl	0x8(%rbx)
  5d934e: 4d 89 be 50 02 00 00         	movq	%r15, 0x250(%r14)
  5d9355: 4d 8b be 58 02 00 00         	movq	0x258(%r14), %r15
  5d935c: 49 39 df                     	cmpq	%rbx, %r15
  5d935f: 74 2a                        	je	0x5d938b <module_init+0x5d936b>
  5d9361: 4d 85 ff                     	testq	%r15, %r15
  5d9364: 49 89 9e 58 02 00 00         	movq	%rbx, 0x258(%r14)
  5d936b: 74 3b                        	je	0x5d93a8 <module_init+0x5d9388>
  5d936d: f0                           	lock
  5d936e: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9372: 75 34                        	jne	0x5d93a8 <module_init+0x5d9388>
  5d9374: 49 8b 07                     	movq	(%r15), %rax
  5d9377: 4c 89 ff                     	movq	%r15, %rdi
  5d937a: ff 10                        	callq	*(%rax)
  5d937c: f0                           	lock
  5d937d: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d9381: 75 25                        	jne	0x5d93a8 <module_init+0x5d9388>
  5d9383: 49 8b 07                     	movq	(%r15), %rax
  5d9386: 4c 89 ff                     	movq	%r15, %rdi
  5d9389: eb 1a                        	jmp	0x5d93a5 <module_init+0x5d9385>
  5d938b: f0                           	lock
  5d938c: ff 4b 08                     	decl	0x8(%rbx)
  5d938f: 75 17                        	jne	0x5d93a8 <module_init+0x5d9388>
  5d9391: 48 8b 03                     	movq	(%rbx), %rax
  5d9394: 48 89 df                     	movq	%rbx, %rdi
  5d9397: ff 10                        	callq	*(%rax)
  5d9399: f0                           	lock
  5d939a: ff 4b 0c                     	decl	0xc(%rbx)
  5d939d: 75 09                        	jne	0x5d93a8 <module_init+0x5d9388>
  5d939f: 48 8b 03                     	movq	(%rbx), %rax
  5d93a2: 48 89 df                     	movq	%rbx, %rdi
  5d93a5: ff 50 10                     	callq	*0x10(%rax)
  5d93a8: f0                           	lock
  5d93a9: ff 4b 08                     	decl	0x8(%rbx)
  5d93ac: 75 17                        	jne	0x5d93c5 <module_init+0x5d93a5>
  5d93ae: 48 8b 03                     	movq	(%rbx), %rax
  5d93b1: 48 89 df                     	movq	%rbx, %rdi
  5d93b4: ff 10                        	callq	*(%rax)
  5d93b6: f0                           	lock
  5d93b7: ff 4b 0c                     	decl	0xc(%rbx)
  5d93ba: 75 09                        	jne	0x5d93c5 <module_init+0x5d93a5>
  5d93bc: 48 8b 03                     	movq	(%rbx), %rax
  5d93bf: 48 89 df                     	movq	%rbx, %rdi
  5d93c2: ff 50 10                     	callq	*0x10(%rax)
  5d93c5: bf 30 04 00 00               	movl	$0x430, %edi            # imm = 0x430
  5d93ca: e8 d1 7f 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d93cf: c5 f8 10 0d 29 14 44 04      	vmovups	0x4441429(%rip), %xmm1  # 0x4a1a800 <plt_log10+0x4a2b0>
  5d93d7: 48 89 c3                     	movq	%rax, %rbx
  5d93da: 48 b8 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rax      # imm = 0x100000001
  5d93e4: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d93e8: 48 89 43 08                  	movq	%rax, 0x8(%rbx)
  5d93ec: 48 8d 05 0d ae 89 05         	leaq	0x589ae0d(%rip), %rax   # 0x5e74200 <plt_log10+0x14a3cb0>
  5d93f3: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d93f7: 48 89 03                     	movq	%rax, (%rbx)
  5d93fa: 48 b8 00 00 00 00 02 00 00 00	movabsq	$0x200000000, %rax      # imm = 0x200000000
  5d9404: c5 fa 7f 43 18               	vmovdqu	%xmm0, 0x18(%rbx)
  5d9409: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5d9410: c5 fa 7f 43 2c               	vmovdqu	%xmm0, 0x2c(%rbx)
  5d9415: 48 89 43 3c                  	movq	%rax, 0x3c(%rbx)
  5d9419: c5 fa 7f 43 44               	vmovdqu	%xmm0, 0x44(%rbx)
  5d941e: 48 89 43 54                  	movq	%rax, 0x54(%rbx)
  5d9422: c5 fa 7f 43 5c               	vmovdqu	%xmm0, 0x5c(%rbx)
  5d9427: 48 89 43 6c                  	movq	%rax, 0x6c(%rbx)
  5d942b: c5 fa 7f 43 74               	vmovdqu	%xmm0, 0x74(%rbx)
  5d9430: 48 89 83 84 00 00 00         	movq	%rax, 0x84(%rbx)
  5d9437: c5 fa 7f 83 8c 00 00 00      	vmovdqu	%xmm0, 0x8c(%rbx)
  5d943f: 48 89 83 9c 00 00 00         	movq	%rax, 0x9c(%rbx)
  5d9446: c5 fa 7f 83 a4 00 00 00      	vmovdqu	%xmm0, 0xa4(%rbx)
  5d944e: 48 89 83 b4 00 00 00         	movq	%rax, 0xb4(%rbx)
  5d9455: c5 fa 7f 83 bc 00 00 00      	vmovdqu	%xmm0, 0xbc(%rbx)
  5d945d: c7 83 cc 00 00 00 00 00 00 00	movl	$0x0, 0xcc(%rbx)
  5d9467: c7 83 d0 00 00 00 02 00 00 00	movl	$0x2, 0xd0(%rbx)
  5d9471: c5 fa 7f 83 d4 00 00 00      	vmovdqu	%xmm0, 0xd4(%rbx)
  5d9479: 48 89 83 e4 00 00 00         	movq	%rax, 0xe4(%rbx)
  5d9480: c5 fa 7f 83 ec 00 00 00      	vmovdqu	%xmm0, 0xec(%rbx)
  5d9488: 48 89 83 fc 00 00 00         	movq	%rax, 0xfc(%rbx)
  5d948f: c5 fa 7f 83 04 01 00 00      	vmovdqu	%xmm0, 0x104(%rbx)
  5d9497: 48 89 83 14 01 00 00         	movq	%rax, 0x114(%rbx)
  5d949e: c5 fa 7f 83 1c 01 00 00      	vmovdqu	%xmm0, 0x11c(%rbx)
  5d94a6: 48 89 83 2c 01 00 00         	movq	%rax, 0x12c(%rbx)
  5d94ad: c5 fa 7f 83 34 01 00 00      	vmovdqu	%xmm0, 0x134(%rbx)
  5d94b5: 48 89 83 44 01 00 00         	movq	%rax, 0x144(%rbx)
  5d94bc: c5 fa 7f 83 4c 01 00 00      	vmovdqu	%xmm0, 0x14c(%rbx)
  5d94c4: 48 89 83 5c 01 00 00         	movq	%rax, 0x15c(%rbx)
  5d94cb: c5 fa 7f 83 64 01 00 00      	vmovdqu	%xmm0, 0x164(%rbx)
  5d94d3: 48 89 83 74 01 00 00         	movq	%rax, 0x174(%rbx)
  5d94da: c5 fa 7f 83 7c 01 00 00      	vmovdqu	%xmm0, 0x17c(%rbx)
  5d94e2: 48 89 83 8c 01 00 00         	movq	%rax, 0x18c(%rbx)
  5d94e9: c5 fa 7f 83 94 01 00 00      	vmovdqu	%xmm0, 0x194(%rbx)
  5d94f1: c7 83 a4 01 00 00 00 00 00 00	movl	$0x0, 0x1a4(%rbx)
  5d94fb: c7 83 a8 01 00 00 02 00 00 00	movl	$0x2, 0x1a8(%rbx)
  5d9505: c5 fa 7f 83 ac 01 00 00      	vmovdqu	%xmm0, 0x1ac(%rbx)
  5d950d: 48 89 83 bc 01 00 00         	movq	%rax, 0x1bc(%rbx)
  5d9514: c5 fa 7f 83 c4 01 00 00      	vmovdqu	%xmm0, 0x1c4(%rbx)
  5d951c: 48 89 83 d4 01 00 00         	movq	%rax, 0x1d4(%rbx)
  5d9523: c5 fa 7f 83 dc 01 00 00      	vmovdqu	%xmm0, 0x1dc(%rbx)
  5d952b: 48 89 83 ec 01 00 00         	movq	%rax, 0x1ec(%rbx)
  5d9532: c5 fa 7f 83 f4 01 00 00      	vmovdqu	%xmm0, 0x1f4(%rbx)
  5d953a: 48 89 83 04 02 00 00         	movq	%rax, 0x204(%rbx)
  5d9541: c5 fa 7f 83 0c 02 00 00      	vmovdqu	%xmm0, 0x20c(%rbx)
  5d9549: 48 89 83 1c 02 00 00         	movq	%rax, 0x21c(%rbx)
  5d9550: c5 fa 7f 83 24 02 00 00      	vmovdqu	%xmm0, 0x224(%rbx)
  5d9558: 48 89 83 34 02 00 00         	movq	%rax, 0x234(%rbx)
  5d955f: 48 89 83 4c 02 00 00         	movq	%rax, 0x24c(%rbx)
  5d9566: 48 8d 05 fb 70 89 05         	leaq	0x58970fb(%rip), %rax   # 0x5e70668 <plt_log10+0x14a0118>
  5d956d: c5 fa 7f 83 3c 02 00 00      	vmovdqu	%xmm0, 0x23c(%rbx)
  5d9575: c5 fa 7f 83 54 02 00 00      	vmovdqu	%xmm0, 0x254(%rbx)
  5d957d: c7 83 64 02 00 00 00 00 00 00	movl	$0x0, 0x264(%rbx)
  5d9587: c7 83 68 02 00 00 02 00 00 00	movl	$0x2, 0x268(%rbx)
  5d9591: c5 fa 7f 83 6c 02 00 00      	vmovdqu	%xmm0, 0x26c(%rbx)
  5d9599: c7 83 7c 02 00 00 00 00 00 00	movl	$0x0, 0x27c(%rbx)
  5d95a3: c7 83 80 02 00 00 02 00 00 00	movl	$0x2, 0x280(%rbx)
  5d95ad: c5 fa 7f 83 84 02 00 00      	vmovdqu	%xmm0, 0x284(%rbx)
  5d95b5: c7 83 94 02 00 00 00 00 00 00	movl	$0x0, 0x294(%rbx)
  5d95bf: c7 83 98 02 00 00 02 00 00 00	movl	$0x2, 0x298(%rbx)
  5d95c9: c5 fa 7f 83 9c 02 00 00      	vmovdqu	%xmm0, 0x29c(%rbx)
  5d95d1: c7 83 ac 02 00 00 00 00 00 00	movl	$0x0, 0x2ac(%rbx)
  5d95db: c7 83 b0 02 00 00 02 00 00 00	movl	$0x2, 0x2b0(%rbx)
  5d95e5: c5 fa 7f 83 b4 02 00 00      	vmovdqu	%xmm0, 0x2b4(%rbx)
  5d95ed: c7 83 c4 02 00 00 00 00 00 00	movl	$0x0, 0x2c4(%rbx)
  5d95f7: c7 83 c8 02 00 00 02 00 00 00	movl	$0x2, 0x2c8(%rbx)
  5d9601: c5 fa 7f 83 cc 02 00 00      	vmovdqu	%xmm0, 0x2cc(%rbx)
  5d9609: c7 83 dc 02 00 00 00 00 00 00	movl	$0x0, 0x2dc(%rbx)
  5d9613: c7 83 e0 02 00 00 02 00 00 00	movl	$0x2, 0x2e0(%rbx)
  5d961d: c5 fa 7f 83 e4 02 00 00      	vmovdqu	%xmm0, 0x2e4(%rbx)
  5d9625: c7 83 f4 02 00 00 00 00 00 00	movl	$0x0, 0x2f4(%rbx)
  5d962f: c7 83 f8 02 00 00 02 00 00 00	movl	$0x2, 0x2f8(%rbx)
  5d9639: c5 fa 7f 83 fc 02 00 00      	vmovdqu	%xmm0, 0x2fc(%rbx)
  5d9641: c7 83 0c 03 00 00 00 00 00 00	movl	$0x0, 0x30c(%rbx)
  5d964b: c7 83 10 03 00 00 02 00 00 00	movl	$0x2, 0x310(%rbx)
  5d9655: c5 fa 7f 83 14 03 00 00      	vmovdqu	%xmm0, 0x314(%rbx)
  5d965d: c7 83 24 03 00 00 00 00 00 00	movl	$0x0, 0x324(%rbx)
  5d9667: c7 83 28 03 00 00 02 00 00 00	movl	$0x2, 0x328(%rbx)
  5d9671: c5 fa 7f 83 2c 03 00 00      	vmovdqu	%xmm0, 0x32c(%rbx)
  5d9679: c7 83 3c 03 00 00 00 00 00 00	movl	$0x0, 0x33c(%rbx)
  5d9683: c7 83 40 03 00 00 02 00 00 00	movl	$0x2, 0x340(%rbx)
  5d968d: c5 fa 7f 83 44 03 00 00      	vmovdqu	%xmm0, 0x344(%rbx)
  5d9695: c7 83 54 03 00 00 00 00 00 00	movl	$0x0, 0x354(%rbx)
  5d969f: c7 83 58 03 00 00 02 00 00 00	movl	$0x2, 0x358(%rbx)
  5d96a9: c5 fa 7f 83 5c 03 00 00      	vmovdqu	%xmm0, 0x35c(%rbx)
  5d96b1: c7 83 6c 03 00 00 00 00 00 00	movl	$0x0, 0x36c(%rbx)
  5d96bb: c7 83 70 03 00 00 02 00 00 00	movl	$0x2, 0x370(%rbx)
  5d96c5: c5 fa 7f 83 74 03 00 00      	vmovdqu	%xmm0, 0x374(%rbx)
  5d96cd: c7 83 84 03 00 00 00 00 00 00	movl	$0x0, 0x384(%rbx)
  5d96d7: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
  5d96db: c5 fa 7f 83 88 03 00 00      	vmovdqu	%xmm0, 0x388(%rbx)
  5d96e3: 48 c7 83 a8 03 00 00 00 00 00 00     	movq	$0x0, 0x3a8(%rbx)
  5d96ee: c5 f8 11 8b b0 03 00 00      	vmovups	%xmm1, 0x3b0(%rbx)
  5d96f6: 48 c7 83 c8 03 00 00 00 00 00 00     	movq	$0x0, 0x3c8(%rbx)
  5d9701: c7 83 d0 03 00 00 00 00 00 00	movl	$0x0, 0x3d0(%rbx)
  5d970b: 4c 89 b3 d8 03 00 00         	movq	%r14, 0x3d8(%rbx)
  5d9712: c5 fa 7f 83 e0 03 00 00      	vmovdqu	%xmm0, 0x3e0(%rbx)
  5d971a: 48 c7 83 00 04 00 00 00 00 00 00     	movq	$0x0, 0x400(%rbx)
  5d9725: c5 f8 11 8b 08 04 00 00      	vmovups	%xmm1, 0x408(%rbx)
  5d972d: 48 c7 83 20 04 00 00 00 00 00 00     	movq	$0x0, 0x420(%rbx)
  5d9738: c7 83 28 04 00 00 00 00 00 00	movl	$0x0, 0x428(%rbx)
  5d9742: f0                           	lock
  5d9743: ff 43 08                     	incl	0x8(%rbx)
  5d9746: 4c 89 bb 78 03 00 00         	movq	%r15, 0x378(%rbx)
  5d974d: 48 39 9b 80 03 00 00         	cmpq	%rbx, 0x380(%rbx)
  5d9754: 74 23                        	je	0x5d9779 <module_init+0x5d9759>
  5d9756: f0                           	lock
  5d9757: ff 43 0c                     	incl	0xc(%rbx)
  5d975a: 48 8b bb 80 03 00 00         	movq	0x380(%rbx), %rdi
  5d9761: 48 85 ff                     	testq	%rdi, %rdi
  5d9764: 74 0c                        	je	0x5d9772 <module_init+0x5d9752>
  5d9766: f0                           	lock
  5d9767: ff 4f 0c                     	decl	0xc(%rdi)
  5d976a: 75 06                        	jne	0x5d9772 <module_init+0x5d9752>
  5d976c: 48 8b 07                     	movq	(%rdi), %rax
  5d976f: ff 50 10                     	callq	*0x10(%rax)
  5d9772: 48 89 9b 80 03 00 00         	movq	%rbx, 0x380(%rbx)
  5d9779: f0                           	lock
  5d977a: ff 4b 08                     	decl	0x8(%rbx)
  5d977d: 75 17                        	jne	0x5d9796 <module_init+0x5d9776>
  5d977f: 48 8b 03                     	movq	(%rbx), %rax
  5d9782: 48 89 df                     	movq	%rbx, %rdi
  5d9785: ff 10                        	callq	*(%rax)
  5d9787: f0                           	lock
  5d9788: ff 4b 0c                     	decl	0xc(%rbx)
  5d978b: 75 09                        	jne	0x5d9796 <module_init+0x5d9776>
  5d978d: 48 8b 03                     	movq	(%rbx), %rax
  5d9790: 48 89 df                     	movq	%rbx, %rdi
  5d9793: ff 50 10                     	callq	*0x10(%rax)
  5d9796: f0                           	lock
  5d9797: ff 43 08                     	incl	0x8(%rbx)
  5d979a: 4d 89 be 60 02 00 00         	movq	%r15, 0x260(%r14)
  5d97a1: 4d 8b be 68 02 00 00         	movq	0x268(%r14), %r15
  5d97a8: 49 39 df                     	cmpq	%rbx, %r15
  5d97ab: 74 2a                        	je	0x5d97d7 <module_init+0x5d97b7>
  5d97ad: 4d 85 ff                     	testq	%r15, %r15
  5d97b0: 49 89 9e 68 02 00 00         	movq	%rbx, 0x268(%r14)
  5d97b7: 74 3b                        	je	0x5d97f4 <module_init+0x5d97d4>
  5d97b9: f0                           	lock
  5d97ba: 41 ff 4f 08                  	decl	0x8(%r15)
  5d97be: 75 34                        	jne	0x5d97f4 <module_init+0x5d97d4>
  5d97c0: 49 8b 07                     	movq	(%r15), %rax
  5d97c3: 4c 89 ff                     	movq	%r15, %rdi
  5d97c6: ff 10                        	callq	*(%rax)
  5d97c8: f0                           	lock
  5d97c9: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d97cd: 75 25                        	jne	0x5d97f4 <module_init+0x5d97d4>
  5d97cf: 49 8b 07                     	movq	(%r15), %rax
  5d97d2: 4c 89 ff                     	movq	%r15, %rdi
  5d97d5: eb 1a                        	jmp	0x5d97f1 <module_init+0x5d97d1>
  5d97d7: f0                           	lock
  5d97d8: ff 4b 08                     	decl	0x8(%rbx)
  5d97db: 75 17                        	jne	0x5d97f4 <module_init+0x5d97d4>
  5d97dd: 48 8b 03                     	movq	(%rbx), %rax
  5d97e0: 48 89 df                     	movq	%rbx, %rdi
  5d97e3: ff 10                        	callq	*(%rax)
  5d97e5: f0                           	lock
  5d97e6: ff 4b 0c                     	decl	0xc(%rbx)
  5d97e9: 75 09                        	jne	0x5d97f4 <module_init+0x5d97d4>
  5d97eb: 48 8b 03                     	movq	(%rbx), %rax
  5d97ee: 48 89 df                     	movq	%rbx, %rdi
  5d97f1: ff 50 10                     	callq	*0x10(%rax)
  5d97f4: f0                           	lock
  5d97f5: ff 4b 08                     	decl	0x8(%rbx)
  5d97f8: 75 17                        	jne	0x5d9811 <module_init+0x5d97f1>
  5d97fa: 48 8b 03                     	movq	(%rbx), %rax
  5d97fd: 48 89 df                     	movq	%rbx, %rdi
  5d9800: ff 10                        	callq	*(%rax)
  5d9802: f0                           	lock
  5d9803: ff 4b 0c                     	decl	0xc(%rbx)
  5d9806: 75 09                        	jne	0x5d9811 <module_init+0x5d97f1>
  5d9808: 48 8b 03                     	movq	(%rbx), %rax
  5d980b: 48 89 df                     	movq	%rbx, %rdi
  5d980e: ff 50 10                     	callq	*0x10(%rax)
  5d9811: bf b8 00 00 00               	movl	$0xb8, %edi
  5d9816: e8 85 7b 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d981b: 49 89 c5                     	movq	%rax, %r13
  5d981e: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d9828: 48 8d 0d f9 a9 89 05         	leaq	0x589a9f9(%rip), %rcx   # 0x5e74228 <plt_log10+0x14a3cd8>
  5d982f: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  5d9833: be 30 00 00 00               	movl	$0x30, %esi
  5d9838: 48 c7 45 c0 00 00 00 00      	movq	$0x0, -0x40(%rbp)
  5d9840: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d9844: c7 45 c8 00 00 00 00         	movl	$0x0, -0x38(%rbp)
  5d984b: 49 89 55 08                  	movq	%rdx, 0x8(%r13)
  5d984f: 49 89 4d 00                  	movq	%rcx, (%r13)
  5d9853: 48 8d 15 fe 58 89 05         	leaq	0x58958fe(%rip), %rdx   # 0x5e6f158 <plt_log10+0x149ec08>
  5d985a: 48 b9 80 00 00 00 ff ff ff ff	movabsq	$-0xffffff80, %rcx      # imm = 0xFFFFFFFF00000080
  5d9864: c4 c1 7a 7f 45 18            	vmovdqu	%xmm0, 0x18(%r13)
  5d986a: 4d 8d 65 10                  	leaq	0x10(%r13), %r12
  5d986e: c5 f9 ef c0                  	vpxor	%xmm0, %xmm0, %xmm0
  5d9872: 4d 8d bd 98 00 00 00         	leaq	0x98(%r13), %r15
  5d9879: 49 89 55 10                  	movq	%rdx, 0x10(%r13)
  5d987d: 49 c7 45 68 00 00 00 00      	movq	$0x0, 0x68(%r13)
  5d9885: 41 c7 45 70 00 00 00 00      	movl	$0x0, 0x70(%r13)
  5d988d: c4 c1 7e 7f 45 28            	vmovdqu	%ymm0, 0x28(%r13)
  5d9893: c4 c1 7e 7f 45 38            	vmovdqu	%ymm0, 0x38(%r13)
  5d9899: 49 89 4d 74                  	movq	%rcx, 0x74(%r13)
  5d989d: 41 c7 45 7c 00 00 00 00      	movl	$0x0, 0x7c(%r13)
  5d98a5: 49 c7 85 88 00 00 00 00 00 00 00     	movq	$0x0, 0x88(%r13)
  5d98b0: 41 c7 85 90 00 00 00 00 00 00 00     	movl	$0x0, 0x90(%r13)
  5d98bb: 49 c7 85 98 00 00 00 00 00 00 00     	movq	$0x0, 0x98(%r13)
  5d98c6: 41 c7 85 a0 00 00 00 00 00 00 00     	movl	$0x0, 0xa0(%r13)
  5d98d1: 49 c7 85 a8 00 00 00 00 00 00 00     	movq	$0x0, 0xa8(%r13)
  5d98dc: 4d 89 b5 b0 00 00 00         	movq	%r14, 0xb0(%r13)
  5d98e3: e8 28 68 a3 ff               	callq	0x10110 <module_init+0x100f0>
  5d98e8: 48 89 c3                     	movq	%rax, %rbx
  5d98eb: 48 8d 0d 4e d9 82 05         	leaq	0x582d94e(%rip), %rcx   # 0x5e07240 <plt_log10+0x1436cf0>
  5d98f2: 48 89 0b                     	movq	%rcx, (%rbx)
  5d98f5: e8 a6 c6 a3 00               	callq	0x1015fa0 <module_init+0x1015f80>
  5d98fa: 48 8d 15 4f a9 89 05         	leaq	0x589a94f(%rip), %rdx   # 0x5e74250 <plt_log10+0x14a3d00>
  5d9901: 48 8d 0d a8 f2 fd ff         	leaq	-0x20d58(%rip), %rcx    # 0x5b8bb0 <module_init+0x5b8b90>
  5d9908: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
  5d990c: 4c 89 ff                     	movq	%r15, %rdi
  5d990f: 48 89 13                     	movq	%rdx, (%rbx)
  5d9912: 4c 89 63 18                  	movq	%r12, 0x18(%rbx)
  5d9916: 48 89 4b 20                  	movq	%rcx, 0x20(%rbx)
  5d991a: 48 c7 43 28 00 00 00 00      	movq	$0x0, 0x28(%rbx)
  5d9922: 48 8d 5d c0                  	leaq	-0x40(%rbp), %rbx
  5d9926: 48 89 de                     	movq	%rbx, %rsi
  5d9929: e8 52 5a a6 ff               	callq	0x3f380 <module_init+0x3f360>
  5d992e: 48 89 df                     	movq	%rbx, %rdi
  5d9931: e8 4a 6c aa ff               	callq	0x80580 <module_init+0x80560>
  5d9936: e8 55 41 b9 00               	callq	0x116da90 <module_init+0x116da70>
  5d993b: c5 fa 10 05 89 19 44 04      	vmovss	0x4441989(%rip), %xmm0  # xmm0 = mem[0],zero,zero,zero
                                                                        # 0x4a1b2cc <plt_log10+0x4ad7c>
  5d9943: 48 89 c7                     	movq	%rax, %rdi
  5d9946: 4c 89 fe                     	movq	%r15, %rsi
  5d9949: e8 72 bd a3 00               	callq	0x10156c0 <module_init+0x10156a0>
  5d994e: 49 89 85 a8 00 00 00         	movq	%rax, 0xa8(%r13)
  5d9955: 49 83 7d 18 00               	cmpq	$0x0, 0x18(%r13)
  5d995a: 74 16                        	je	0x5d9972 <module_init+0x5d9952>
  5d995c: 49 8b 45 20                  	movq	0x20(%r13), %rax
  5d9960: 48 85 c0                     	testq	%rax, %rax
  5d9963: 74 0d                        	je	0x5d9972 <module_init+0x5d9952>
  5d9965: 0f ae f0                     	mfence
  5d9968: 8b 40 08                     	movl	0x8(%rax), %eax
  5d996b: 0f ae f0                     	mfence
  5d996e: 85 c0                        	testl	%eax, %eax
  5d9970: 7f 4e                        	jg	0x5d99c0 <module_init+0x5d99a0>
  5d9972: f0                           	lock
  5d9973: 41 ff 45 08                  	incl	0x8(%r13)
  5d9977: 4d 89 65 18                  	movq	%r12, 0x18(%r13)
  5d997b: 4d 39 6d 20                  	cmpq	%r13, 0x20(%r13)
  5d997f: 74 1e                        	je	0x5d999f <module_init+0x5d997f>
  5d9981: f0                           	lock
  5d9982: 41 ff 45 0c                  	incl	0xc(%r13)
  5d9986: 49 8b 7d 20                  	movq	0x20(%r13), %rdi
  5d998a: 48 85 ff                     	testq	%rdi, %rdi
  5d998d: 74 0c                        	je	0x5d999b <module_init+0x5d997b>
  5d998f: f0                           	lock
  5d9990: ff 4f 0c                     	decl	0xc(%rdi)
  5d9993: 75 06                        	jne	0x5d999b <module_init+0x5d997b>
  5d9995: 48 8b 07                     	movq	(%rdi), %rax
  5d9998: ff 50 10                     	callq	*0x10(%rax)
  5d999b: 4d 89 6d 20                  	movq	%r13, 0x20(%r13)
  5d999f: f0                           	lock
  5d99a0: 41 ff 4d 08                  	decl	0x8(%r13)
  5d99a4: 75 1a                        	jne	0x5d99c0 <module_init+0x5d99a0>
  5d99a6: 49 8b 45 00                  	movq	(%r13), %rax
  5d99aa: 4c 89 ef                     	movq	%r13, %rdi
  5d99ad: ff 10                        	callq	*(%rax)
  5d99af: f0                           	lock
  5d99b0: 41 ff 4d 0c                  	decl	0xc(%r13)
  5d99b4: 75 0a                        	jne	0x5d99c0 <module_init+0x5d99a0>
  5d99b6: 49 8b 45 00                  	movq	(%r13), %rax
  5d99ba: 4c 89 ef                     	movq	%r13, %rdi
  5d99bd: ff 50 10                     	callq	*0x10(%rax)
  5d99c0: f0                           	lock
  5d99c1: 41 ff 45 08                  	incl	0x8(%r13)
  5d99c5: 4d 89 a6 70 02 00 00         	movq	%r12, 0x270(%r14)
  5d99cc: 4d 8b be 78 02 00 00         	movq	0x278(%r14), %r15
  5d99d3: 4d 39 ef                     	cmpq	%r13, %r15
  5d99d6: 74 31                        	je	0x5d9a09 <module_init+0x5d99e9>
  5d99d8: 4c 8b 25 69 96 f9 05         	movq	0x5f99669(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d99df: 4d 85 ff                     	testq	%r15, %r15
  5d99e2: 4d 89 ae 78 02 00 00         	movq	%r13, 0x278(%r14)
  5d99e9: 74 46                        	je	0x5d9a31 <module_init+0x5d9a11>
  5d99eb: f0                           	lock
  5d99ec: 41 ff 4f 08                  	decl	0x8(%r15)
  5d99f0: 75 3f                        	jne	0x5d9a31 <module_init+0x5d9a11>
  5d99f2: 49 8b 07                     	movq	(%r15), %rax
  5d99f5: 4c 89 ff                     	movq	%r15, %rdi
  5d99f8: ff 10                        	callq	*(%rax)
  5d99fa: f0                           	lock
  5d99fb: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d99ff: 75 30                        	jne	0x5d9a31 <module_init+0x5d9a11>
  5d9a01: 49 8b 07                     	movq	(%r15), %rax
  5d9a04: 4c 89 ff                     	movq	%r15, %rdi
  5d9a07: eb 25                        	jmp	0x5d9a2e <module_init+0x5d9a0e>
  5d9a09: f0                           	lock
  5d9a0a: 41 ff 4d 08                  	decl	0x8(%r13)
  5d9a0e: 4c 8b 25 33 96 f9 05         	movq	0x5f99633(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5d9a15: 75 1a                        	jne	0x5d9a31 <module_init+0x5d9a11>
  5d9a17: 49 8b 45 00                  	movq	(%r13), %rax
  5d9a1b: 4c 89 ef                     	movq	%r13, %rdi
  5d9a1e: ff 10                        	callq	*(%rax)
  5d9a20: f0                           	lock
  5d9a21: 41 ff 4d 0c                  	decl	0xc(%r13)
  5d9a25: 75 0a                        	jne	0x5d9a31 <module_init+0x5d9a11>
  5d9a27: 49 8b 45 00                  	movq	(%r13), %rax
  5d9a2b: 4c 89 ef                     	movq	%r13, %rdi
  5d9a2e: ff 50 10                     	callq	*0x10(%rax)
  5d9a31: f0                           	lock
  5d9a32: 41 ff 4d 08                  	decl	0x8(%r13)
  5d9a36: 75 1a                        	jne	0x5d9a52 <module_init+0x5d9a32>
  5d9a38: 49 8b 45 00                  	movq	(%r13), %rax
  5d9a3c: 4c 89 ef                     	movq	%r13, %rdi
  5d9a3f: ff 10                        	callq	*(%rax)
  5d9a41: f0                           	lock
  5d9a42: 41 ff 4d 0c                  	decl	0xc(%r13)
  5d9a46: 75 0a                        	jne	0x5d9a52 <module_init+0x5d9a32>
  5d9a48: 49 8b 45 00                  	movq	(%r13), %rax
  5d9a4c: 4c 89 ef                     	movq	%r13, %rdi
  5d9a4f: ff 50 10                     	callq	*0x10(%rax)
  5d9a52: bf d0 00 00 00               	movl	$0xd0, %edi
  5d9a57: e8 44 79 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9a5c: 48 89 c3                     	movq	%rax, %rbx
  5d9a5f: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d9a69: 48 8d 0d 50 a8 89 05         	leaq	0x589a850(%rip), %rcx   # 0x5e742c0 <plt_log10+0x14a3d70>
  5d9a70: 48 8d 35 69 63 89 05         	leaq	0x5896369(%rip), %rsi   # 0x5e6fde0 <plt_log10+0x149f890>
  5d9a77: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9a7b: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d9a7f: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9a82: 48 b9 00 00 00 00 02 00 00 00	movabsq	$0x200000000, %rcx      # imm = 0x200000000
  5d9a8c: 48 ba 80 00 00 00 ff ff ff ff	movabsq	$-0xffffff80, %rdx      # imm = 0xFFFFFFFF00000080
  5d9a96: c5 f8 11 43 18               	vmovups	%xmm0, 0x18(%rbx)
  5d9a9b: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5d9aa2: 48 8d 43 10                  	leaq	0x10(%rbx), %rax
  5d9aa6: c5 f8 11 43 2c               	vmovups	%xmm0, 0x2c(%rbx)
  5d9aab: 48 89 4b 3c                  	movq	%rcx, 0x3c(%rbx)
  5d9aaf: c5 f8 11 43 44               	vmovups	%xmm0, 0x44(%rbx)
  5d9ab4: 48 89 4b 54                  	movq	%rcx, 0x54(%rbx)
  5d9ab8: c5 f8 11 43 5c               	vmovups	%xmm0, 0x5c(%rbx)
  5d9abd: c7 43 6c 00 00 00 00         	movl	$0x0, 0x6c(%rbx)
  5d9ac4: 48 c7 43 70 02 00 00 00      	movq	$0x2, 0x70(%rbx)
  5d9acc: 48 89 73 10                  	movq	%rsi, 0x10(%rbx)
  5d9ad0: 4c 89 73 78                  	movq	%r14, 0x78(%rbx)
  5d9ad4: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9ad8: c5 fc 11 83 b0 00 00 00      	vmovups	%ymm0, 0xb0(%rbx)
  5d9ae0: c5 fc 11 83 a0 00 00 00      	vmovups	%ymm0, 0xa0(%rbx)
  5d9ae8: c5 fc 11 83 80 00 00 00      	vmovups	%ymm0, 0x80(%rbx)
  5d9af0: 48 89 93 ac 00 00 00         	movq	%rdx, 0xac(%rbx)
  5d9af7: c7 83 b4 00 00 00 00 00 00 00	movl	$0x0, 0xb4(%rbx)
  5d9b01: 48 c7 83 c0 00 00 00 00 00 00 00     	movq	$0x0, 0xc0(%rbx)
  5d9b0c: c7 83 c8 00 00 00 00 00 00 00	movl	$0x0, 0xc8(%rbx)
  5d9b16: f0                           	lock
  5d9b17: ff 43 08                     	incl	0x8(%rbx)
  5d9b1a: 49 89 86 d0 02 00 00         	movq	%rax, 0x2d0(%r14)
  5d9b21: 4d 8b be d8 02 00 00         	movq	0x2d8(%r14), %r15
  5d9b28: 49 39 df                     	cmpq	%rbx, %r15
  5d9b2b: 74 2e                        	je	0x5d9b5b <module_init+0x5d9b3b>
  5d9b2d: 4c 8b 6d 80                  	movq	-0x80(%rbp), %r13
  5d9b31: 4d 85 ff                     	testq	%r15, %r15
  5d9b34: 49 89 9e d8 02 00 00         	movq	%rbx, 0x2d8(%r14)
  5d9b3b: 74 3f                        	je	0x5d9b7c <module_init+0x5d9b5c>
  5d9b3d: f0                           	lock
  5d9b3e: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9b42: 75 38                        	jne	0x5d9b7c <module_init+0x5d9b5c>
  5d9b44: 49 8b 07                     	movq	(%r15), %rax
  5d9b47: 4c 89 ff                     	movq	%r15, %rdi
  5d9b4a: ff 10                        	callq	*(%rax)
  5d9b4c: f0                           	lock
  5d9b4d: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d9b51: 75 29                        	jne	0x5d9b7c <module_init+0x5d9b5c>
  5d9b53: 49 8b 07                     	movq	(%r15), %rax
  5d9b56: 4c 89 ff                     	movq	%r15, %rdi
  5d9b59: eb 1e                        	jmp	0x5d9b79 <module_init+0x5d9b59>
  5d9b5b: f0                           	lock
  5d9b5c: ff 4b 08                     	decl	0x8(%rbx)
  5d9b5f: 4c 8b 6d 80                  	movq	-0x80(%rbp), %r13
  5d9b63: 75 17                        	jne	0x5d9b7c <module_init+0x5d9b5c>
  5d9b65: 48 8b 03                     	movq	(%rbx), %rax
  5d9b68: 48 89 df                     	movq	%rbx, %rdi
  5d9b6b: ff 10                        	callq	*(%rax)
  5d9b6d: f0                           	lock
  5d9b6e: ff 4b 0c                     	decl	0xc(%rbx)
  5d9b71: 75 09                        	jne	0x5d9b7c <module_init+0x5d9b5c>
  5d9b73: 48 8b 03                     	movq	(%rbx), %rax
  5d9b76: 48 89 df                     	movq	%rbx, %rdi
  5d9b79: ff 50 10                     	callq	*0x10(%rax)
  5d9b7c: f0                           	lock
  5d9b7d: ff 4b 08                     	decl	0x8(%rbx)
  5d9b80: 75 17                        	jne	0x5d9b99 <module_init+0x5d9b79>
  5d9b82: 48 8b 03                     	movq	(%rbx), %rax
  5d9b85: 48 89 df                     	movq	%rbx, %rdi
  5d9b88: ff 10                        	callq	*(%rax)
  5d9b8a: f0                           	lock
  5d9b8b: ff 4b 0c                     	decl	0xc(%rbx)
  5d9b8e: 75 09                        	jne	0x5d9b99 <module_init+0x5d9b79>
  5d9b90: 48 8b 03                     	movq	(%rbx), %rax
  5d9b93: 48 89 df                     	movq	%rbx, %rdi
  5d9b96: ff 50 10                     	callq	*0x10(%rax)
  5d9b99: bf 90 00 00 00               	movl	$0x90, %edi
  5d9b9e: e8 fd 77 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9ba3: 48 89 c3                     	movq	%rax, %rbx
  5d9ba6: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d9bb0: 48 8d 0d 31 a7 89 05         	leaq	0x589a731(%rip), %rcx   # 0x5e742e8 <plt_log10+0x14a3d98>
  5d9bb7: 4c 89 f6                     	movq	%r14, %rsi
  5d9bba: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d9bbe: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d9bc2: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9bc5: 4c 89 ff                     	movq	%r15, %rdi
  5d9bc8: e8 c3 78 08 00               	callq	0x661490 <module_init+0x661470>
  5d9bcd: 48 8d 05 34 56 89 05         	leaq	0x5895634(%rip), %rax   # 0x5e6f208 <plt_log10+0x149ecb8>
  5d9bd4: bf b2 00 00 00               	movl	$0xb2, %edi
  5d9bd9: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
  5d9bdd: 4c 89 b3 88 00 00 00         	movq	%r14, 0x88(%rbx)
  5d9be4: e8 f7 18 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9be9: bf ab 00 00 00               	movl	$0xab, %edi
  5d9bee: e8 ed 18 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9bf3: bf a2 00 00 00               	movl	$0xa2, %edi
  5d9bf8: e8 e3 18 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9bfd: bf ac 00 00 00               	movl	$0xac, %edi
  5d9c02: e8 d9 18 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5d9c07: e8 44 2e 3f 04               	callq	0x49cca50 <plt_sceCommonDialogInitialize>
  5d9c0c: f0                           	lock
  5d9c0d: ff 43 08                     	incl	0x8(%rbx)
  5d9c10: 4d 89 be f0 02 00 00         	movq	%r15, 0x2f0(%r14)
  5d9c17: 4d 8b be f8 02 00 00         	movq	0x2f8(%r14), %r15
  5d9c1e: 49 39 df                     	cmpq	%rbx, %r15
  5d9c21: 74 2a                        	je	0x5d9c4d <module_init+0x5d9c2d>
  5d9c23: 4d 85 ff                     	testq	%r15, %r15
  5d9c26: 49 89 9e f8 02 00 00         	movq	%rbx, 0x2f8(%r14)
  5d9c2d: 74 3b                        	je	0x5d9c6a <module_init+0x5d9c4a>
  5d9c2f: f0                           	lock
  5d9c30: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9c34: 75 34                        	jne	0x5d9c6a <module_init+0x5d9c4a>
  5d9c36: 49 8b 07                     	movq	(%r15), %rax
  5d9c39: 4c 89 ff                     	movq	%r15, %rdi
  5d9c3c: ff 10                        	callq	*(%rax)
  5d9c3e: f0                           	lock
  5d9c3f: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d9c43: 75 25                        	jne	0x5d9c6a <module_init+0x5d9c4a>
  5d9c45: 49 8b 07                     	movq	(%r15), %rax
  5d9c48: 4c 89 ff                     	movq	%r15, %rdi
  5d9c4b: eb 1a                        	jmp	0x5d9c67 <module_init+0x5d9c47>
  5d9c4d: f0                           	lock
  5d9c4e: ff 4b 08                     	decl	0x8(%rbx)
  5d9c51: 75 17                        	jne	0x5d9c6a <module_init+0x5d9c4a>
  5d9c53: 48 8b 03                     	movq	(%rbx), %rax
  5d9c56: 48 89 df                     	movq	%rbx, %rdi
  5d9c59: ff 10                        	callq	*(%rax)
  5d9c5b: f0                           	lock
  5d9c5c: ff 4b 0c                     	decl	0xc(%rbx)
  5d9c5f: 75 09                        	jne	0x5d9c6a <module_init+0x5d9c4a>
  5d9c61: 48 8b 03                     	movq	(%rbx), %rax
  5d9c64: 48 89 df                     	movq	%rbx, %rdi
  5d9c67: ff 50 10                     	callq	*0x10(%rax)
  5d9c6a: f0                           	lock
  5d9c6b: ff 4b 08                     	decl	0x8(%rbx)
  5d9c6e: 75 17                        	jne	0x5d9c87 <module_init+0x5d9c67>
  5d9c70: 48 8b 03                     	movq	(%rbx), %rax
  5d9c73: 48 89 df                     	movq	%rbx, %rdi
  5d9c76: ff 10                        	callq	*(%rax)
  5d9c78: f0                           	lock
  5d9c79: ff 4b 0c                     	decl	0xc(%rbx)
  5d9c7c: 75 09                        	jne	0x5d9c87 <module_init+0x5d9c67>
  5d9c7e: 48 8b 03                     	movq	(%rbx), %rax
  5d9c81: 48 89 df                     	movq	%rbx, %rdi
  5d9c84: ff 50 10                     	callq	*0x10(%rax)
  5d9c87: bf a0 00 00 00               	movl	$0xa0, %edi
  5d9c8c: e8 0f 77 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9c91: c5 f8 10 0d 67 0b 44 04      	vmovups	0x4440b67(%rip), %xmm1  # 0x4a1a800 <plt_log10+0x4a2b0>
  5d9c99: 48 89 c3                     	movq	%rax, %rbx
  5d9c9c: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d9ca6: 48 8d 0d 63 a6 89 05         	leaq	0x589a663(%rip), %rcx   # 0x5e74310 <plt_log10+0x14a3dc0>
  5d9cad: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9cb1: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d9cb5: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9cb8: 48 8d 0d 01 6d 89 05         	leaq	0x5896d01(%rip), %rcx   # 0x5e709c0 <plt_log10+0x14a0470>
  5d9cbf: c5 f8 11 43 18               	vmovups	%xmm0, 0x18(%rbx)
  5d9cc4: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5d9ccb: 48 8d 43 10                  	leaq	0x10(%rbx), %rax
  5d9ccf: c5 f8 11 43 2c               	vmovups	%xmm0, 0x2c(%rbx)
  5d9cd4: c7 43 3c 00 00 00 00         	movl	$0x0, 0x3c(%rbx)
  5d9cdb: 48 c7 43 40 02 00 00 00      	movq	$0x2, 0x40(%rbx)
  5d9ce3: 48 89 4b 10                  	movq	%rcx, 0x10(%rbx)
  5d9ce7: 4c 89 73 48                  	movq	%r14, 0x48(%rbx)
  5d9ceb: c5 f8 11 43 50               	vmovups	%xmm0, 0x50(%rbx)
  5d9cf0: 48 c7 43 70 00 00 00 00      	movq	$0x0, 0x70(%rbx)
  5d9cf8: c5 f8 11 4b 78               	vmovups	%xmm1, 0x78(%rbx)
  5d9cfd: 48 c7 83 90 00 00 00 00 00 00 00     	movq	$0x0, 0x90(%rbx)
  5d9d08: c7 83 98 00 00 00 00 00 00 00	movl	$0x0, 0x98(%rbx)
  5d9d12: f0                           	lock
  5d9d13: ff 43 08                     	incl	0x8(%rbx)
  5d9d16: 49 89 86 10 03 00 00         	movq	%rax, 0x310(%r14)
  5d9d1d: 4d 8b be 18 03 00 00         	movq	0x318(%r14), %r15
  5d9d24: 49 39 df                     	cmpq	%rbx, %r15
  5d9d27: 74 2a                        	je	0x5d9d53 <module_init+0x5d9d33>
  5d9d29: 4d 85 ff                     	testq	%r15, %r15
  5d9d2c: 49 89 9e 18 03 00 00         	movq	%rbx, 0x318(%r14)
  5d9d33: 74 3b                        	je	0x5d9d70 <module_init+0x5d9d50>
  5d9d35: f0                           	lock
  5d9d36: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9d3a: 75 34                        	jne	0x5d9d70 <module_init+0x5d9d50>
  5d9d3c: 49 8b 07                     	movq	(%r15), %rax
  5d9d3f: 4c 89 ff                     	movq	%r15, %rdi
  5d9d42: ff 10                        	callq	*(%rax)
  5d9d44: f0                           	lock
  5d9d45: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d9d49: 75 25                        	jne	0x5d9d70 <module_init+0x5d9d50>
  5d9d4b: 49 8b 07                     	movq	(%r15), %rax
  5d9d4e: 4c 89 ff                     	movq	%r15, %rdi
  5d9d51: eb 1a                        	jmp	0x5d9d6d <module_init+0x5d9d4d>
  5d9d53: f0                           	lock
  5d9d54: ff 4b 08                     	decl	0x8(%rbx)
  5d9d57: 75 17                        	jne	0x5d9d70 <module_init+0x5d9d50>
  5d9d59: 48 8b 03                     	movq	(%rbx), %rax
  5d9d5c: 48 89 df                     	movq	%rbx, %rdi
  5d9d5f: ff 10                        	callq	*(%rax)
  5d9d61: f0                           	lock
  5d9d62: ff 4b 0c                     	decl	0xc(%rbx)
  5d9d65: 75 09                        	jne	0x5d9d70 <module_init+0x5d9d50>
  5d9d67: 48 8b 03                     	movq	(%rbx), %rax
  5d9d6a: 48 89 df                     	movq	%rbx, %rdi
  5d9d6d: ff 50 10                     	callq	*0x10(%rax)
  5d9d70: f0                           	lock
  5d9d71: ff 4b 08                     	decl	0x8(%rbx)
  5d9d74: 75 17                        	jne	0x5d9d8d <module_init+0x5d9d6d>
  5d9d76: 48 8b 03                     	movq	(%rbx), %rax
  5d9d79: 48 89 df                     	movq	%rbx, %rdi
  5d9d7c: ff 10                        	callq	*(%rax)
  5d9d7e: f0                           	lock
  5d9d7f: ff 4b 0c                     	decl	0xc(%rbx)
  5d9d82: 75 09                        	jne	0x5d9d8d <module_init+0x5d9d6d>
  5d9d84: 48 8b 03                     	movq	(%rbx), %rax
  5d9d87: 48 89 df                     	movq	%rbx, %rdi
  5d9d8a: ff 50 10                     	callq	*0x10(%rax)
  5d9d8d: 48 8b 85 78 ff ff ff         	movq	-0x88(%rbp), %rax
  5d9d94: 80 38 00                     	cmpb	$0x0, (%rax)
  5d9d97: 0f 84 af 02 00 00            	je	0x5da04c <module_init+0x5da02c>
  5d9d9d: bf a8 00 00 00               	movl	$0xa8, %edi
  5d9da2: e8 f9 75 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9da7: 48 89 c3                     	movq	%rax, %rbx
  5d9daa: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5d9db4: 48 8d 0d 7d a5 89 05         	leaq	0x589a57d(%rip), %rcx   # 0x5e74338 <plt_log10+0x14a3de8>
  5d9dbb: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9dbf: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5d9dc3: 48 89 0b                     	movq	%rcx, (%rbx)
  5d9dc6: 48 8d 15 b3 59 89 05         	leaq	0x58959b3(%rip), %rdx   # 0x5e6f780 <plt_log10+0x149f230>
  5d9dcd: 48 b9 80 00 00 00 ff ff ff ff	movabsq	$-0xffffff80, %rcx      # imm = 0xFFFFFFFF00000080
  5d9dd7: c5 f8 11 43 18               	vmovups	%xmm0, 0x18(%rbx)
  5d9ddc: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5d9de3: c5 f8 11 43 2c               	vmovups	%xmm0, 0x2c(%rbx)
  5d9de8: c7 43 3c 00 00 00 00         	movl	$0x0, 0x3c(%rbx)
  5d9def: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9df3: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d9df7: 48 89 53 10                  	movq	%rdx, 0x10(%rbx)
  5d9dfb: 4c 89 73 40                  	movq	%r14, 0x40(%rbx)
  5d9dff: 48 c7 43 78 00 00 00 00      	movq	$0x0, 0x78(%rbx)
  5d9e07: c7 83 80 00 00 00 00 00 00 00	movl	$0x0, 0x80(%rbx)
  5d9e11: c5 fc 11 43 48               	vmovups	%ymm0, 0x48(%rbx)
  5d9e16: 48 89 8b 84 00 00 00         	movq	%rcx, 0x84(%rbx)
  5d9e1d: c7 83 8c 00 00 00 00 00 00 00	movl	$0x0, 0x8c(%rbx)
  5d9e27: 48 c7 83 98 00 00 00 00 00 00 00     	movq	$0x0, 0x98(%rbx)
  5d9e32: c7 83 a0 00 00 00 00 00 00 00	movl	$0x0, 0xa0(%rbx)
  5d9e3c: f0                           	lock
  5d9e3d: ff 43 08                     	incl	0x8(%rbx)
  5d9e40: 4c 89 7b 30                  	movq	%r15, 0x30(%rbx)
  5d9e44: 48 39 5b 38                  	cmpq	%rbx, 0x38(%rbx)
  5d9e48: 74 1d                        	je	0x5d9e67 <module_init+0x5d9e47>
  5d9e4a: f0                           	lock
  5d9e4b: ff 43 0c                     	incl	0xc(%rbx)
  5d9e4e: 48 8b 7b 38                  	movq	0x38(%rbx), %rdi
  5d9e52: 48 85 ff                     	testq	%rdi, %rdi
  5d9e55: 74 0c                        	je	0x5d9e63 <module_init+0x5d9e43>
  5d9e57: f0                           	lock
  5d9e58: ff 4f 0c                     	decl	0xc(%rdi)
  5d9e5b: 75 06                        	jne	0x5d9e63 <module_init+0x5d9e43>
  5d9e5d: 48 8b 07                     	movq	(%rdi), %rax
  5d9e60: ff 50 10                     	callq	*0x10(%rax)
  5d9e63: 48 89 5b 38                  	movq	%rbx, 0x38(%rbx)
  5d9e67: f0                           	lock
  5d9e68: ff 4b 08                     	decl	0x8(%rbx)
  5d9e6b: 75 17                        	jne	0x5d9e84 <module_init+0x5d9e64>
  5d9e6d: 48 8b 03                     	movq	(%rbx), %rax
  5d9e70: 48 89 df                     	movq	%rbx, %rdi
  5d9e73: ff 10                        	callq	*(%rax)
  5d9e75: f0                           	lock
  5d9e76: ff 4b 0c                     	decl	0xc(%rbx)
  5d9e79: 75 09                        	jne	0x5d9e84 <module_init+0x5d9e64>
  5d9e7b: 48 8b 03                     	movq	(%rbx), %rax
  5d9e7e: 48 89 df                     	movq	%rbx, %rdi
  5d9e81: ff 50 10                     	callq	*0x10(%rax)
  5d9e84: f0                           	lock
  5d9e85: ff 43 08                     	incl	0x8(%rbx)
  5d9e88: 4d 89 be a0 02 00 00         	movq	%r15, 0x2a0(%r14)
  5d9e8f: 4d 8b be a8 02 00 00         	movq	0x2a8(%r14), %r15
  5d9e96: 49 39 df                     	cmpq	%rbx, %r15
  5d9e99: 74 2a                        	je	0x5d9ec5 <module_init+0x5d9ea5>
  5d9e9b: 4d 85 ff                     	testq	%r15, %r15
  5d9e9e: 49 89 9e a8 02 00 00         	movq	%rbx, 0x2a8(%r14)
  5d9ea5: 74 3b                        	je	0x5d9ee2 <module_init+0x5d9ec2>
  5d9ea7: f0                           	lock
  5d9ea8: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9eac: 75 34                        	jne	0x5d9ee2 <module_init+0x5d9ec2>
  5d9eae: 49 8b 07                     	movq	(%r15), %rax
  5d9eb1: 4c 89 ff                     	movq	%r15, %rdi
  5d9eb4: ff 10                        	callq	*(%rax)
  5d9eb6: f0                           	lock
  5d9eb7: 41 ff 4f 0c                  	decl	0xc(%r15)
  5d9ebb: 75 25                        	jne	0x5d9ee2 <module_init+0x5d9ec2>
  5d9ebd: 49 8b 07                     	movq	(%r15), %rax
  5d9ec0: 4c 89 ff                     	movq	%r15, %rdi
  5d9ec3: eb 1a                        	jmp	0x5d9edf <module_init+0x5d9ebf>
  5d9ec5: f0                           	lock
  5d9ec6: ff 4b 08                     	decl	0x8(%rbx)
  5d9ec9: 75 17                        	jne	0x5d9ee2 <module_init+0x5d9ec2>
  5d9ecb: 48 8b 03                     	movq	(%rbx), %rax
  5d9ece: 48 89 df                     	movq	%rbx, %rdi
  5d9ed1: ff 10                        	callq	*(%rax)
  5d9ed3: f0                           	lock
  5d9ed4: ff 4b 0c                     	decl	0xc(%rbx)
  5d9ed7: 75 09                        	jne	0x5d9ee2 <module_init+0x5d9ec2>
  5d9ed9: 48 8b 03                     	movq	(%rbx), %rax
  5d9edc: 48 89 df                     	movq	%rbx, %rdi
  5d9edf: ff 50 10                     	callq	*0x10(%rax)
  5d9ee2: f0                           	lock
  5d9ee3: ff 4b 08                     	decl	0x8(%rbx)
  5d9ee6: 75 17                        	jne	0x5d9eff <module_init+0x5d9edf>
  5d9ee8: 48 8b 03                     	movq	(%rbx), %rax
  5d9eeb: 48 89 df                     	movq	%rbx, %rdi
  5d9eee: ff 10                        	callq	*(%rax)
  5d9ef0: f0                           	lock
  5d9ef1: ff 4b 0c                     	decl	0xc(%rbx)
  5d9ef4: 75 09                        	jne	0x5d9eff <module_init+0x5d9edf>
  5d9ef6: 48 8b 03                     	movq	(%rbx), %rax
  5d9ef9: 48 89 df                     	movq	%rbx, %rdi
  5d9efc: ff 50 10                     	callq	*0x10(%rax)
  5d9eff: bf a0 00 00 00               	movl	$0xa0, %edi
  5d9f04: e8 97 74 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5d9f09: c5 f8 10 0d ef 08 44 04      	vmovups	0x44408ef(%rip), %xmm1  # 0x4a1a800 <plt_log10+0x4a2b0>
  5d9f11: 48 89 c3                     	movq	%rax, %rbx
  5d9f14: 48 b9 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rcx      # imm = 0x100000001
  5d9f1e: 48 8d 15 3b a4 89 05         	leaq	0x589a43b(%rip), %rdx   # 0x5e74360 <plt_log10+0x14a3e10>
  5d9f25: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5d9f29: 48 89 4b 08                  	movq	%rcx, 0x8(%rbx)
  5d9f2d: 48 8d 0d 9c 56 89 05         	leaq	0x589569c(%rip), %rcx   # 0x5e6f5d0 <plt_log10+0x149f080>
  5d9f34: 48 89 13                     	movq	%rdx, (%rbx)
  5d9f37: c5 f8 11 43 18               	vmovups	%xmm0, 0x18(%rbx)
  5d9f3c: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5d9f43: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5d9f47: c5 f8 11 43 2c               	vmovups	%xmm0, 0x2c(%rbx)
  5d9f4c: c7 43 3c 00 00 00 00         	movl	$0x0, 0x3c(%rbx)
  5d9f53: 48 89 4b 10                  	movq	%rcx, 0x10(%rbx)
  5d9f57: 4c 89 73 40                  	movq	%r14, 0x40(%rbx)
  5d9f5b: c5 f8 11 43 48               	vmovups	%xmm0, 0x48(%rbx)
  5d9f60: 48 c7 43 68 00 00 00 00      	movq	$0x0, 0x68(%rbx)
  5d9f68: c5 f8 11 4b 70               	vmovups	%xmm1, 0x70(%rbx)
  5d9f6d: 48 c7 83 88 00 00 00 00 00 00 00     	movq	$0x0, 0x88(%rbx)
  5d9f78: c7 83 90 00 00 00 00 00 00 00	movl	$0x0, 0x90(%rbx)
  5d9f82: c6 83 98 00 00 00 00         	movb	$0x0, 0x98(%rbx)
  5d9f89: f0                           	lock
  5d9f8a: ff 43 08                     	incl	0x8(%rbx)
  5d9f8d: 4c 89 7b 30                  	movq	%r15, 0x30(%rbx)
  5d9f91: 48 39 5b 38                  	cmpq	%rbx, 0x38(%rbx)
  5d9f95: 74 1d                        	je	0x5d9fb4 <module_init+0x5d9f94>
  5d9f97: f0                           	lock
  5d9f98: ff 43 0c                     	incl	0xc(%rbx)
  5d9f9b: 48 8b 7b 38                  	movq	0x38(%rbx), %rdi
  5d9f9f: 48 85 ff                     	testq	%rdi, %rdi
  5d9fa2: 74 0c                        	je	0x5d9fb0 <module_init+0x5d9f90>
  5d9fa4: f0                           	lock
  5d9fa5: ff 4f 0c                     	decl	0xc(%rdi)
  5d9fa8: 75 06                        	jne	0x5d9fb0 <module_init+0x5d9f90>
  5d9faa: 48 8b 07                     	movq	(%rdi), %rax
  5d9fad: ff 50 10                     	callq	*0x10(%rax)
  5d9fb0: 48 89 5b 38                  	movq	%rbx, 0x38(%rbx)
  5d9fb4: f0                           	lock
  5d9fb5: ff 4b 08                     	decl	0x8(%rbx)
  5d9fb8: 75 17                        	jne	0x5d9fd1 <module_init+0x5d9fb1>
  5d9fba: 48 8b 03                     	movq	(%rbx), %rax
  5d9fbd: 48 89 df                     	movq	%rbx, %rdi
  5d9fc0: ff 10                        	callq	*(%rax)
  5d9fc2: f0                           	lock
  5d9fc3: ff 4b 0c                     	decl	0xc(%rbx)
  5d9fc6: 75 09                        	jne	0x5d9fd1 <module_init+0x5d9fb1>
  5d9fc8: 48 8b 03                     	movq	(%rbx), %rax
  5d9fcb: 48 89 df                     	movq	%rbx, %rdi
  5d9fce: ff 50 10                     	callq	*0x10(%rax)
  5d9fd1: f0                           	lock
  5d9fd2: ff 43 08                     	incl	0x8(%rbx)
  5d9fd5: 4d 89 be 90 02 00 00         	movq	%r15, 0x290(%r14)
  5d9fdc: 4d 8b be 98 02 00 00         	movq	0x298(%r14), %r15
  5d9fe3: 49 39 df                     	cmpq	%rbx, %r15
  5d9fe6: 74 2a                        	je	0x5da012 <module_init+0x5d9ff2>
  5d9fe8: 4d 85 ff                     	testq	%r15, %r15
  5d9feb: 49 89 9e 98 02 00 00         	movq	%rbx, 0x298(%r14)
  5d9ff2: 74 3b                        	je	0x5da02f <module_init+0x5da00f>
  5d9ff4: f0                           	lock
  5d9ff5: 41 ff 4f 08                  	decl	0x8(%r15)
  5d9ff9: 75 34                        	jne	0x5da02f <module_init+0x5da00f>
  5d9ffb: 49 8b 07                     	movq	(%r15), %rax
  5d9ffe: 4c 89 ff                     	movq	%r15, %rdi
  5da001: ff 10                        	callq	*(%rax)
  5da003: f0                           	lock
  5da004: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da008: 75 25                        	jne	0x5da02f <module_init+0x5da00f>
  5da00a: 49 8b 07                     	movq	(%r15), %rax
  5da00d: 4c 89 ff                     	movq	%r15, %rdi
  5da010: eb 1a                        	jmp	0x5da02c <module_init+0x5da00c>
  5da012: f0                           	lock
  5da013: ff 4b 08                     	decl	0x8(%rbx)
  5da016: 75 17                        	jne	0x5da02f <module_init+0x5da00f>
  5da018: 48 8b 03                     	movq	(%rbx), %rax
  5da01b: 48 89 df                     	movq	%rbx, %rdi
  5da01e: ff 10                        	callq	*(%rax)
  5da020: f0                           	lock
  5da021: ff 4b 0c                     	decl	0xc(%rbx)
  5da024: 75 09                        	jne	0x5da02f <module_init+0x5da00f>
  5da026: 48 8b 03                     	movq	(%rbx), %rax
  5da029: 48 89 df                     	movq	%rbx, %rdi
  5da02c: ff 50 10                     	callq	*0x10(%rax)
  5da02f: f0                           	lock
  5da030: ff 4b 08                     	decl	0x8(%rbx)
  5da033: 75 17                        	jne	0x5da04c <module_init+0x5da02c>
  5da035: 48 8b 03                     	movq	(%rbx), %rax
  5da038: 48 89 df                     	movq	%rbx, %rdi
  5da03b: ff 10                        	callq	*(%rax)
  5da03d: f0                           	lock
  5da03e: ff 4b 0c                     	decl	0xc(%rbx)
  5da041: 75 09                        	jne	0x5da04c <module_init+0x5da02c>
  5da043: 48 8b 03                     	movq	(%rbx), %rax
  5da046: 48 89 df                     	movq	%rbx, %rdi
  5da049: ff 50 10                     	callq	*0x10(%rax)
  5da04c: 48 8b 45 88                  	movq	-0x78(%rbp), %rax
  5da050: 80 38 00                     	cmpb	$0x0, (%rax)
  5da053: 0f 84 a7 01 00 00            	je	0x5da200 <module_init+0x5da1e0>
  5da059: bf 88 01 00 00               	movl	$0x188, %edi            # imm = 0x188
  5da05e: e8 3d 73 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da063: c5 f8 10 0d 95 07 44 04      	vmovups	0x4440795(%rip), %xmm1  # 0x4a1a800 <plt_log10+0x4a2b0>
  5da06b: 48 89 c3                     	movq	%rax, %rbx
  5da06e: 48 b8 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rax      # imm = 0x100000001
  5da078: bf ad 00 00 00               	movl	$0xad, %edi
  5da07d: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5da081: 48 89 43 08                  	movq	%rax, 0x8(%rbx)
  5da085: 48 8d 05 fc a2 89 05         	leaq	0x589a2fc(%rip), %rax   # 0x5e74388 <plt_log10+0x14a3e38>
  5da08c: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5da090: 48 89 03                     	movq	%rax, (%rbx)
  5da093: 48 8d 05 fe 50 89 05         	leaq	0x58950fe(%rip), %rax   # 0x5e6f198 <plt_log10+0x149ec48>
  5da09a: c5 f8 11 43 18               	vmovups	%xmm0, 0x18(%rbx)
  5da09f: 48 c7 43 28 02 00 00 00      	movq	$0x2, 0x28(%rbx)
  5da0a7: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
  5da0ab: 48 8d 05 9e 7c 89 05         	leaq	0x5897c9e(%rip), %rax   # 0x5e71d50 <plt_log10+0x14a1800>
  5da0b2: 4c 89 73 30                  	movq	%r14, 0x30(%rbx)
  5da0b6: 66 c7 43 38 00 00            	movw	$0x0, 0x38(%rbx)
  5da0bc: c5 f8 11 43 40               	vmovups	%xmm0, 0x40(%rbx)
  5da0c1: 48 c7 43 60 00 00 00 00      	movq	$0x0, 0x60(%rbx)
  5da0c9: c5 f8 11 4b 68               	vmovups	%xmm1, 0x68(%rbx)
  5da0ce: 48 c7 83 80 00 00 00 00 00 00 00     	movq	$0x0, 0x80(%rbx)
  5da0d9: c7 83 88 00 00 00 00 00 00 00	movl	$0x0, 0x88(%rbx)
  5da0e3: 48 89 83 90 00 00 00         	movq	%rax, 0x90(%rbx)
  5da0ea: c5 f8 11 83 98 00 00 00      	vmovups	%xmm0, 0x98(%rbx)
  5da0f2: 48 c7 83 b8 00 00 00 00 00 00 00     	movq	$0x0, 0xb8(%rbx)
  5da0fd: c5 f8 11 8b c0 00 00 00      	vmovups	%xmm1, 0xc0(%rbx)
  5da105: 48 c7 83 d8 00 00 00 00 00 00 00     	movq	$0x0, 0xd8(%rbx)
  5da110: c7 83 e0 00 00 00 00 00 00 00	movl	$0x0, 0xe0(%rbx)
  5da11a: c5 f8 11 83 e8 00 00 00      	vmovups	%xmm0, 0xe8(%rbx)
  5da122: 48 c7 83 08 01 00 00 00 00 00 00     	movq	$0x0, 0x108(%rbx)
  5da12d: c5 f8 11 8b 10 01 00 00      	vmovups	%xmm1, 0x110(%rbx)
  5da135: 48 c7 83 28 01 00 00 00 00 00 00     	movq	$0x0, 0x128(%rbx)
  5da140: c7 83 30 01 00 00 00 00 00 00	movl	$0x0, 0x130(%rbx)
  5da14a: c5 f8 11 83 38 01 00 00      	vmovups	%xmm0, 0x138(%rbx)
  5da152: 48 c7 83 58 01 00 00 00 00 00 00     	movq	$0x0, 0x158(%rbx)
  5da15d: c5 f8 11 8b 60 01 00 00      	vmovups	%xmm1, 0x160(%rbx)
  5da165: 48 c7 83 78 01 00 00 00 00 00 00     	movq	$0x0, 0x178(%rbx)
  5da170: c7 83 80 01 00 00 00 00 00 00	movl	$0x0, 0x180(%rbx)
  5da17a: e8 61 13 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5da17f: 85 c0                        	testl	%eax, %eax
  5da181: 0f 94 43 38                  	sete	0x38(%rbx)
  5da185: f0                           	lock
  5da186: ff 43 08                     	incl	0x8(%rbx)
  5da189: 4d 89 be 80 02 00 00         	movq	%r15, 0x280(%r14)
  5da190: 4d 8b be 88 02 00 00         	movq	0x288(%r14), %r15
  5da197: 49 39 df                     	cmpq	%rbx, %r15
  5da19a: 74 2a                        	je	0x5da1c6 <module_init+0x5da1a6>
  5da19c: 4d 85 ff                     	testq	%r15, %r15
  5da19f: 49 89 9e 88 02 00 00         	movq	%rbx, 0x288(%r14)
  5da1a6: 74 3b                        	je	0x5da1e3 <module_init+0x5da1c3>
  5da1a8: f0                           	lock
  5da1a9: 41 ff 4f 08                  	decl	0x8(%r15)
  5da1ad: 75 34                        	jne	0x5da1e3 <module_init+0x5da1c3>
  5da1af: 49 8b 07                     	movq	(%r15), %rax
  5da1b2: 4c 89 ff                     	movq	%r15, %rdi
  5da1b5: ff 10                        	callq	*(%rax)
  5da1b7: f0                           	lock
  5da1b8: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da1bc: 75 25                        	jne	0x5da1e3 <module_init+0x5da1c3>
  5da1be: 49 8b 07                     	movq	(%r15), %rax
  5da1c1: 4c 89 ff                     	movq	%r15, %rdi
  5da1c4: eb 1a                        	jmp	0x5da1e0 <module_init+0x5da1c0>
  5da1c6: f0                           	lock
  5da1c7: ff 4b 08                     	decl	0x8(%rbx)
  5da1ca: 75 17                        	jne	0x5da1e3 <module_init+0x5da1c3>
  5da1cc: 48 8b 03                     	movq	(%rbx), %rax
  5da1cf: 48 89 df                     	movq	%rbx, %rdi
  5da1d2: ff 10                        	callq	*(%rax)
  5da1d4: f0                           	lock
  5da1d5: ff 4b 0c                     	decl	0xc(%rbx)
  5da1d8: 75 09                        	jne	0x5da1e3 <module_init+0x5da1c3>
  5da1da: 48 8b 03                     	movq	(%rbx), %rax
  5da1dd: 48 89 df                     	movq	%rbx, %rdi
  5da1e0: ff 50 10                     	callq	*0x10(%rax)
  5da1e3: f0                           	lock
  5da1e4: ff 4b 08                     	decl	0x8(%rbx)
  5da1e7: 75 17                        	jne	0x5da200 <module_init+0x5da1e0>
  5da1e9: 48 8b 03                     	movq	(%rbx), %rax
  5da1ec: 48 89 df                     	movq	%rbx, %rdi
  5da1ef: ff 10                        	callq	*(%rax)
  5da1f1: f0                           	lock
  5da1f2: ff 4b 0c                     	decl	0xc(%rbx)
  5da1f5: 75 09                        	jne	0x5da200 <module_init+0x5da1e0>
  5da1f7: 48 8b 03                     	movq	(%rbx), %rax
  5da1fa: 48 89 df                     	movq	%rbx, %rdi
  5da1fd: ff 50 10                     	callq	*0x10(%rax)
  5da200: 48 8b 85 70 ff ff ff         	movq	-0x90(%rbp), %rax
  5da207: 80 38 00                     	cmpb	$0x0, (%rax)
  5da20a: 0f 84 af 00 00 00            	je	0x5da2bf <module_init+0x5da29f>
  5da210: bf 58 00 00 00               	movl	$0x58, %edi
  5da215: e8 86 71 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da21a: 48 89 c3                     	movq	%rax, %rbx
  5da21d: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5da227: 48 8d 0d 82 a1 89 05         	leaq	0x589a182(%rip), %rcx   # 0x5e743b0 <plt_log10+0x14a3e60>
  5da22e: 4c 89 f6                     	movq	%r14, %rsi
  5da231: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5da235: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5da239: 48 89 0b                     	movq	%rcx, (%rbx)
  5da23c: 4c 89 ff                     	movq	%r15, %rdi
  5da23f: e8 cc 99 fe ff               	callq	0x5c3c10 <module_init+0x5c3bf0>
  5da244: f0                           	lock
  5da245: ff 43 08                     	incl	0x8(%rbx)
  5da248: 4d 89 be c0 02 00 00         	movq	%r15, 0x2c0(%r14)
  5da24f: 4d 8b be c8 02 00 00         	movq	0x2c8(%r14), %r15
  5da256: 49 39 df                     	cmpq	%rbx, %r15
  5da259: 74 2a                        	je	0x5da285 <module_init+0x5da265>
  5da25b: 4d 85 ff                     	testq	%r15, %r15
  5da25e: 49 89 9e c8 02 00 00         	movq	%rbx, 0x2c8(%r14)
  5da265: 74 3b                        	je	0x5da2a2 <module_init+0x5da282>
  5da267: f0                           	lock
  5da268: 41 ff 4f 08                  	decl	0x8(%r15)
  5da26c: 75 34                        	jne	0x5da2a2 <module_init+0x5da282>
  5da26e: 49 8b 07                     	movq	(%r15), %rax
  5da271: 4c 89 ff                     	movq	%r15, %rdi
  5da274: ff 10                        	callq	*(%rax)
  5da276: f0                           	lock
  5da277: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da27b: 75 25                        	jne	0x5da2a2 <module_init+0x5da282>
  5da27d: 49 8b 07                     	movq	(%r15), %rax
  5da280: 4c 89 ff                     	movq	%r15, %rdi
  5da283: eb 1a                        	jmp	0x5da29f <module_init+0x5da27f>
  5da285: f0                           	lock
  5da286: ff 4b 08                     	decl	0x8(%rbx)
  5da289: 75 17                        	jne	0x5da2a2 <module_init+0x5da282>
  5da28b: 48 8b 03                     	movq	(%rbx), %rax
  5da28e: 48 89 df                     	movq	%rbx, %rdi
  5da291: ff 10                        	callq	*(%rax)
  5da293: f0                           	lock
  5da294: ff 4b 0c                     	decl	0xc(%rbx)
  5da297: 75 09                        	jne	0x5da2a2 <module_init+0x5da282>
  5da299: 48 8b 03                     	movq	(%rbx), %rax
  5da29c: 48 89 df                     	movq	%rbx, %rdi
  5da29f: ff 50 10                     	callq	*0x10(%rax)
  5da2a2: f0                           	lock
  5da2a3: ff 4b 08                     	decl	0x8(%rbx)
  5da2a6: 75 17                        	jne	0x5da2bf <module_init+0x5da29f>
  5da2a8: 48 8b 03                     	movq	(%rbx), %rax
  5da2ab: 48 89 df                     	movq	%rbx, %rdi
  5da2ae: ff 10                        	callq	*(%rax)
  5da2b0: f0                           	lock
  5da2b1: ff 4b 0c                     	decl	0xc(%rbx)
  5da2b4: 75 09                        	jne	0x5da2bf <module_init+0x5da29f>
  5da2b6: 48 8b 03                     	movq	(%rbx), %rax
  5da2b9: 48 89 df                     	movq	%rbx, %rdi
  5da2bc: ff 50 10                     	callq	*0x10(%rax)
  5da2bf: 48 8b 85 68 ff ff ff         	movq	-0x98(%rbp), %rax
  5da2c6: 80 38 00                     	cmpb	$0x0, (%rax)
  5da2c9: 0f 84 af 00 00 00            	je	0x5da37e <module_init+0x5da35e>
  5da2cf: bf 00 02 00 00               	movl	$0x200, %edi            # imm = 0x200
  5da2d4: e8 c7 70 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da2d9: 48 89 c3                     	movq	%rax, %rbx
  5da2dc: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5da2e6: 48 8d 0d eb a0 89 05         	leaq	0x589a0eb(%rip), %rcx   # 0x5e743d8 <plt_log10+0x14a3e88>
  5da2ed: 4c 89 f6                     	movq	%r14, %rsi
  5da2f0: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5da2f4: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5da2f8: 48 89 0b                     	movq	%rcx, (%rbx)
  5da2fb: 4c 89 ff                     	movq	%r15, %rdi
  5da2fe: e8 5d 97 ff ff               	callq	0x5d3a60 <module_init+0x5d3a40>
  5da303: f0                           	lock
  5da304: ff 43 08                     	incl	0x8(%rbx)
  5da307: 4d 89 be 00 03 00 00         	movq	%r15, 0x300(%r14)
  5da30e: 4d 8b be 08 03 00 00         	movq	0x308(%r14), %r15
  5da315: 49 39 df                     	cmpq	%rbx, %r15
  5da318: 74 2a                        	je	0x5da344 <module_init+0x5da324>
  5da31a: 4d 85 ff                     	testq	%r15, %r15
  5da31d: 49 89 9e 08 03 00 00         	movq	%rbx, 0x308(%r14)
  5da324: 74 3b                        	je	0x5da361 <module_init+0x5da341>
  5da326: f0                           	lock
  5da327: 41 ff 4f 08                  	decl	0x8(%r15)
  5da32b: 75 34                        	jne	0x5da361 <module_init+0x5da341>
  5da32d: 49 8b 07                     	movq	(%r15), %rax
  5da330: 4c 89 ff                     	movq	%r15, %rdi
  5da333: ff 10                        	callq	*(%rax)
  5da335: f0                           	lock
  5da336: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da33a: 75 25                        	jne	0x5da361 <module_init+0x5da341>
  5da33c: 49 8b 07                     	movq	(%r15), %rax
  5da33f: 4c 89 ff                     	movq	%r15, %rdi
  5da342: eb 1a                        	jmp	0x5da35e <module_init+0x5da33e>
  5da344: f0                           	lock
  5da345: ff 4b 08                     	decl	0x8(%rbx)
  5da348: 75 17                        	jne	0x5da361 <module_init+0x5da341>
  5da34a: 48 8b 03                     	movq	(%rbx), %rax
  5da34d: 48 89 df                     	movq	%rbx, %rdi
  5da350: ff 10                        	callq	*(%rax)
  5da352: f0                           	lock
  5da353: ff 4b 0c                     	decl	0xc(%rbx)
  5da356: 75 09                        	jne	0x5da361 <module_init+0x5da341>
  5da358: 48 8b 03                     	movq	(%rbx), %rax
  5da35b: 48 89 df                     	movq	%rbx, %rdi
  5da35e: ff 50 10                     	callq	*0x10(%rax)
  5da361: f0                           	lock
  5da362: ff 4b 08                     	decl	0x8(%rbx)
  5da365: 75 17                        	jne	0x5da37e <module_init+0x5da35e>
  5da367: 48 8b 03                     	movq	(%rbx), %rax
  5da36a: 48 89 df                     	movq	%rbx, %rdi
  5da36d: ff 10                        	callq	*(%rax)
  5da36f: f0                           	lock
  5da370: ff 4b 0c                     	decl	0xc(%rbx)
  5da373: 75 09                        	jne	0x5da37e <module_init+0x5da35e>
  5da375: 48 8b 03                     	movq	(%rbx), %rax
  5da378: 48 89 df                     	movq	%rbx, %rdi
  5da37b: ff 50 10                     	callq	*0x10(%rax)
  5da37e: 48 8b 85 60 ff ff ff         	movq	-0xa0(%rbp), %rax
  5da385: 80 38 00                     	cmpb	$0x0, (%rax)
  5da388: 0f 84 bb 00 00 00            	je	0x5da449 <module_init+0x5da429>
  5da38e: bf 58 03 00 00               	movl	$0x358, %edi            # imm = 0x358
  5da393: e8 08 70 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da398: 48 89 c3                     	movq	%rax, %rbx
  5da39b: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5da3a5: 48 8d 0d 54 a0 89 05         	leaq	0x589a054(%rip), %rcx   # 0x5e74400 <plt_log10+0x14a3eb0>
  5da3ac: 4c 89 f6                     	movq	%r14, %rsi
  5da3af: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5da3b3: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5da3b7: 48 89 0b                     	movq	%rcx, (%rbx)
  5da3ba: 4c 89 ff                     	movq	%r15, %rdi
  5da3bd: e8 9e 52 07 00               	callq	0x64f660 <module_init+0x64f640>
  5da3c2: f0                           	lock
  5da3c3: ff 43 08                     	incl	0x8(%rbx)
  5da3c6: 4d 89 be b0 02 00 00         	movq	%r15, 0x2b0(%r14)
  5da3cd: 4d 8b be b8 02 00 00         	movq	0x2b8(%r14), %r15
  5da3d4: 49 39 df                     	cmpq	%rbx, %r15
  5da3d7: 74 2a                        	je	0x5da403 <module_init+0x5da3e3>
  5da3d9: 4d 85 ff                     	testq	%r15, %r15
  5da3dc: 49 89 9e b8 02 00 00         	movq	%rbx, 0x2b8(%r14)
  5da3e3: 74 3b                        	je	0x5da420 <module_init+0x5da400>
  5da3e5: f0                           	lock
  5da3e6: 41 ff 4f 08                  	decl	0x8(%r15)
  5da3ea: 75 34                        	jne	0x5da420 <module_init+0x5da400>
  5da3ec: 49 8b 07                     	movq	(%r15), %rax
  5da3ef: 4c 89 ff                     	movq	%r15, %rdi
  5da3f2: ff 10                        	callq	*(%rax)
  5da3f4: f0                           	lock
  5da3f5: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da3f9: 75 25                        	jne	0x5da420 <module_init+0x5da400>
  5da3fb: 49 8b 07                     	movq	(%r15), %rax
  5da3fe: 4c 89 ff                     	movq	%r15, %rdi
  5da401: eb 1a                        	jmp	0x5da41d <module_init+0x5da3fd>
  5da403: f0                           	lock
  5da404: ff 4b 08                     	decl	0x8(%rbx)
  5da407: 75 17                        	jne	0x5da420 <module_init+0x5da400>
  5da409: 48 8b 03                     	movq	(%rbx), %rax
  5da40c: 48 89 df                     	movq	%rbx, %rdi
  5da40f: ff 10                        	callq	*(%rax)
  5da411: f0                           	lock
  5da412: ff 4b 0c                     	decl	0xc(%rbx)
  5da415: 75 09                        	jne	0x5da420 <module_init+0x5da400>
  5da417: 48 8b 03                     	movq	(%rbx), %rax
  5da41a: 48 89 df                     	movq	%rbx, %rdi
  5da41d: ff 50 10                     	callq	*0x10(%rax)
  5da420: f0                           	lock
  5da421: ff 4b 08                     	decl	0x8(%rbx)
  5da424: 75 17                        	jne	0x5da43d <module_init+0x5da41d>
  5da426: 48 8b 03                     	movq	(%rbx), %rax
  5da429: 48 89 df                     	movq	%rbx, %rdi
  5da42c: ff 10                        	callq	*(%rax)
  5da42e: f0                           	lock
  5da42f: ff 4b 0c                     	decl	0xc(%rbx)
  5da432: 75 09                        	jne	0x5da43d <module_init+0x5da41d>
  5da434: 48 8b 03                     	movq	(%rbx), %rax
  5da437: 48 89 df                     	movq	%rbx, %rdi
  5da43a: ff 50 10                     	callq	*0x10(%rax)
  5da43d: 49 8b be b0 02 00 00         	movq	0x2b0(%r14), %rdi
  5da444: e8 87 05 00 00               	callq	0x5da9d0 <module_init+0x5da9b0>
  5da449: 48 8b 85 58 ff ff ff         	movq	-0xa8(%rbp), %rax
  5da450: 80 38 00                     	cmpb	$0x0, (%rax)
  5da453: 0f 84 8d 02 00 00            	je	0x5da6e6 <module_init+0x5da6c6>
  5da459: bf 10 02 00 00               	movl	$0x210, %edi            # imm = 0x210
  5da45e: e8 3d 6f 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da463: c5 f8 10 05 95 03 44 04      	vmovups	0x4440395(%rip), %xmm0  # 0x4a1a800 <plt_log10+0x4a2b0>
  5da46b: 48 89 c3                     	movq	%rax, %rbx
  5da46e: 48 b8 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rax      # imm = 0x100000001
  5da478: c5 f0 57 c9                  	vxorps	%xmm1, %xmm1, %xmm1
  5da47c: 48 8b 15 55 8f f9 05         	movq	0x5f98f55(%rip), %rdx   # 0x65733d8 <plt_log10+0x1ba2e88>
  5da483: 45 31 ed                     	xorl	%r13d, %r13d
  5da486: 48 89 43 08                  	movq	%rax, 0x8(%rbx)
  5da48a: 48 8d 05 97 9f 89 05         	leaq	0x5899f97(%rip), %rax   # 0x5e74428 <plt_log10+0x14a3ed8>
  5da491: 48 8d 8b c0 01 00 00         	leaq	0x1c0(%rbx), %rcx
  5da498: 4c 8d bb b8 01 00 00         	leaq	0x1b8(%rbx), %r15
  5da49f: 4c 8d a3 c8 01 00 00         	leaq	0x1c8(%rbx), %r12
  5da4a6: 48 89 03                     	movq	%rax, (%rbx)
  5da4a9: 48 b8 00 00 00 00 02 00 00 00	movabsq	$0x200000000, %rax      # imm = 0x200000000
  5da4b3: c5 f8 11 4b 18               	vmovups	%xmm1, 0x18(%rbx)
  5da4b8: c7 43 28 02 00 00 00         	movl	$0x2, 0x28(%rbx)
  5da4bf: 48 89 4d 88                  	movq	%rcx, -0x78(%rbp)
  5da4c3: 48 8b 0d 26 8f f9 05         	movq	0x5f98f26(%rip), %rcx   # 0x65733f0 <plt_log10+0x1ba2ea0>
  5da4ca: c5 f8 11 4b 2c               	vmovups	%xmm1, 0x2c(%rbx)
  5da4cf: 48 89 43 3c                  	movq	%rax, 0x3c(%rbx)
  5da4d3: 48 8d 05 1e 6f 89 05         	leaq	0x5896f1e(%rip), %rax   # 0x5e713f8 <plt_log10+0x14a0ea8>
  5da4da: c5 f8 11 4b 44               	vmovups	%xmm1, 0x44(%rbx)
  5da4df: c7 43 54 00 00 00 00         	movl	$0x0, 0x54(%rbx)
  5da4e6: 48 c7 43 58 02 00 00 00      	movq	$0x2, 0x58(%rbx)
  5da4ee: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
  5da4f2: 4c 89 73 60                  	movq	%r14, 0x60(%rbx)
  5da4f6: c5 f8 11 4b 68               	vmovups	%xmm1, 0x68(%rbx)
  5da4fb: 48 c7 83 88 00 00 00 00 00 00 00     	movq	$0x0, 0x88(%rbx)
  5da506: 48 8d 43 10                  	leaq	0x10(%rbx), %rax
  5da50a: c5 f8 11 83 90 00 00 00      	vmovups	%xmm0, 0x90(%rbx)
  5da512: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5da516: c5 fc c2 c0 0f               	vcmptrueps	%ymm0, %ymm0, %ymm0
  5da51b: 48 89 85 78 ff ff ff         	movq	%rax, -0x88(%rbp)
  5da522: 48 8d 05 67 a3 89 05         	leaq	0x589a367(%rip), %rax   # 0x5e74890 <plt_log10+0x14a4340>
  5da529: 48 c7 83 a8 00 00 00 00 00 00 00     	movq	$0x0, 0xa8(%rbx)
  5da534: c7 83 b0 00 00 00 00 00 00 00	movl	$0x0, 0xb0(%rbx)
  5da53e: c5 fc 11 83 b8 00 00 00      	vmovups	%ymm0, 0xb8(%rbx)
  5da546: c5 fc 11 83 d8 00 00 00      	vmovups	%ymm0, 0xd8(%rbx)
  5da54e: c5 fc 11 83 f8 00 00 00      	vmovups	%ymm0, 0xf8(%rbx)
  5da556: c5 fc 11 83 18 01 00 00      	vmovups	%ymm0, 0x118(%rbx)
  5da55e: c5 fc 11 83 38 01 00 00      	vmovups	%ymm0, 0x138(%rbx)
  5da566: c5 fc 11 83 58 01 00 00      	vmovups	%ymm0, 0x158(%rbx)
  5da56e: c5 fc 11 83 78 01 00 00      	vmovups	%ymm0, 0x178(%rbx)
  5da576: c5 fc 11 83 98 01 00 00      	vmovups	%ymm0, 0x198(%rbx)
  5da57e: 48 89 83 b8 01 00 00         	movq	%rax, 0x1b8(%rbx)
  5da585: c5 f8 11 8b c0 01 00 00      	vmovups	%xmm1, 0x1c0(%rbx)
  5da58d: c7 83 d0 01 00 00 00 00 00 00	movl	$0x0, 0x1d0(%rbx)
  5da597: 48 89 83 d8 01 00 00         	movq	%rax, 0x1d8(%rbx)
  5da59e: c5 f8 11 8b e0 01 00 00      	vmovups	%xmm1, 0x1e0(%rbx)
  5da5a6: c7 83 f0 01 00 00 00 00 00 00	movl	$0x0, 0x1f0(%rbx)
  5da5b0: 48 89 93 f8 01 00 00         	movq	%rdx, 0x1f8(%rbx)
  5da5b7: 48 89 8b 00 02 00 00         	movq	%rcx, 0x200(%rbx)
  5da5be: eb 29                        	jmp	0x5da5e9 <module_init+0x5da5c9>
  5da5c0: 48 8b 4d 88                  	movq	-0x78(%rbp), %rcx
  5da5c4: 49 89 04 24                  	movq	%rax, (%r12)
  5da5c8: 48 89 01                     	movq	%rax, (%rcx)
  5da5cb: 4c 89 ff                     	movq	%r15, %rdi
  5da5ce: 8b b3 d0 01 00 00            	movl	0x1d0(%rbx), %esi
  5da5d4: 48 8b 83 b8 01 00 00         	movq	0x1b8(%rbx), %rax
  5da5db: ff c6                        	incl	%esi
  5da5dd: ff 50 10                     	callq	*0x10(%rax)
  5da5e0: 41 ff c5                     	incl	%r13d
  5da5e3: 41 83 fd 20                  	cmpl	$0x20, %r13d
  5da5e7: 74 2c                        	je	0x5da615 <module_init+0x5da5f5>
  5da5e9: bf 18 00 00 00               	movl	$0x18, %edi
  5da5ee: e8 ad 6d 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da5f3: 49 8b 0c 24                  	movq	(%r12), %rcx
  5da5f7: 44 89 28                     	movl	%r13d, (%rax)
  5da5fa: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5da5fe: c5 f8 11 40 08               	vmovups	%xmm0, 0x8(%rax)
  5da603: 48 85 c9                     	testq	%rcx, %rcx
  5da606: 74 b8                        	je	0x5da5c0 <module_init+0x5da5a0>
  5da608: 48 89 41 08                  	movq	%rax, 0x8(%rcx)
  5da60c: 48 89 48 10                  	movq	%rcx, 0x10(%rax)
  5da610: 4c 89 e1                     	movq	%r12, %rcx
  5da613: eb b3                        	jmp	0x5da5c8 <module_init+0x5da5a8>
  5da615: f0                           	lock
  5da616: ff 43 08                     	incl	0x8(%rbx)
  5da619: 48 8b 85 78 ff ff ff         	movq	-0x88(%rbp), %rax
  5da620: 49 89 86 e0 02 00 00         	movq	%rax, 0x2e0(%r14)
  5da627: 4d 8b be e8 02 00 00         	movq	0x2e8(%r14), %r15
  5da62e: 49 39 df                     	cmpq	%rbx, %r15
  5da631: 74 35                        	je	0x5da668 <module_init+0x5da648>
  5da633: 4c 8b 25 0e 8a f9 05         	movq	0x5f98a0e(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5da63a: 4c 8b 6d 80                  	movq	-0x80(%rbp), %r13
  5da63e: 4d 85 ff                     	testq	%r15, %r15
  5da641: 49 89 9e e8 02 00 00         	movq	%rbx, 0x2e8(%r14)
  5da648: 74 46                        	je	0x5da690 <module_init+0x5da670>
  5da64a: f0                           	lock
  5da64b: 41 ff 4f 08                  	decl	0x8(%r15)
  5da64f: 75 3f                        	jne	0x5da690 <module_init+0x5da670>
  5da651: 49 8b 07                     	movq	(%r15), %rax
  5da654: 4c 89 ff                     	movq	%r15, %rdi
  5da657: ff 10                        	callq	*(%rax)
  5da659: f0                           	lock
  5da65a: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da65e: 75 30                        	jne	0x5da690 <module_init+0x5da670>
  5da660: 49 8b 07                     	movq	(%r15), %rax
  5da663: 4c 89 ff                     	movq	%r15, %rdi
  5da666: eb 25                        	jmp	0x5da68d <module_init+0x5da66d>
  5da668: f0                           	lock
  5da669: ff 4b 08                     	decl	0x8(%rbx)
  5da66c: 4c 8b 25 d5 89 f9 05         	movq	0x5f989d5(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5da673: 4c 8b 6d 80                  	movq	-0x80(%rbp), %r13
  5da677: 75 17                        	jne	0x5da690 <module_init+0x5da670>
  5da679: 48 8b 03                     	movq	(%rbx), %rax
  5da67c: 48 89 df                     	movq	%rbx, %rdi
  5da67f: ff 10                        	callq	*(%rax)
  5da681: f0                           	lock
  5da682: ff 4b 0c                     	decl	0xc(%rbx)
  5da685: 75 09                        	jne	0x5da690 <module_init+0x5da670>
  5da687: 48 8b 03                     	movq	(%rbx), %rax
  5da68a: 48 89 df                     	movq	%rbx, %rdi
  5da68d: ff 50 10                     	callq	*0x10(%rax)
  5da690: f0                           	lock
  5da691: ff 4b 08                     	decl	0x8(%rbx)
  5da694: 75 17                        	jne	0x5da6ad <module_init+0x5da68d>
  5da696: 48 8b 03                     	movq	(%rbx), %rax
  5da699: 48 89 df                     	movq	%rbx, %rdi
  5da69c: ff 10                        	callq	*(%rax)
  5da69e: f0                           	lock
  5da69f: ff 4b 0c                     	decl	0xc(%rbx)
  5da6a2: 75 09                        	jne	0x5da6ad <module_init+0x5da68d>
  5da6a4: 48 8b 03                     	movq	(%rbx), %rax
  5da6a7: 48 89 df                     	movq	%rbx, %rdi
  5da6aa: ff 50 10                     	callq	*0x10(%rax)
  5da6ad: 49 8b 9e e0 02 00 00         	movq	0x2e0(%r14), %rbx
  5da6b4: bf 2c 00 00 00               	movl	$0x2c, %edi
  5da6b9: e8 22 0e 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5da6be: 85 c0                        	testl	%eax, %eax
  5da6c0: 75 24                        	jne	0x5da6e6 <module_init+0x5da6c6>
  5da6c2: 48 8b 05 0f 8d f9 05         	movq	0x5f98d0f(%rip), %rax   # 0x65733d8 <plt_log10+0x1ba2e88>
  5da6c9: 48 8b 0d 20 8d f9 05         	movq	0x5f98d20(%rip), %rcx   # 0x65733f0 <plt_log10+0x1ba2ea0>
  5da6d0: 48 89 df                     	movq	%rbx, %rdi
  5da6d3: 48 89 83 e8 01 00 00         	movq	%rax, 0x1e8(%rbx)
  5da6da: 48 89 8b f0 01 00 00         	movq	%rcx, 0x1f0(%rbx)
  5da6e1: e8 4a 87 00 00               	callq	0x5e2e30 <module_init+0x5e2e10>
  5da6e6: 41 80 7d 00 00               	cmpb	$0x0, (%r13)
  5da6eb: 0f 84 ff 00 00 00            	je	0x5da7f0 <module_init+0x5da7d0>
  5da6f1: bf a0 00 00 00               	movl	$0xa0, %edi
  5da6f6: e8 a5 6c 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da6fb: 48 89 c3                     	movq	%rax, %rbx
  5da6fe: 48 ba 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rdx      # imm = 0x100000001
  5da708: 48 8d 0d 41 9d 89 05         	leaq	0x5899d41(%rip), %rcx   # 0x5e74450 <plt_log10+0x14a3f00>
  5da70f: 4c 89 f6                     	movq	%r14, %rsi
  5da712: 4c 8d 7b 10                  	leaq	0x10(%rbx), %r15
  5da716: 48 89 53 08                  	movq	%rdx, 0x8(%rbx)
  5da71a: 48 89 0b                     	movq	%rcx, (%rbx)
  5da71d: 4c 89 ff                     	movq	%r15, %rdi
  5da720: e8 cb b6 08 00               	callq	0x665df0 <module_init+0x665dd0>
  5da725: f0                           	lock
  5da726: ff 43 08                     	incl	0x8(%rbx)
  5da729: 4d 89 be 20 03 00 00         	movq	%r15, 0x320(%r14)
  5da730: 4d 8b be 28 03 00 00         	movq	0x328(%r14), %r15
  5da737: 49 39 df                     	cmpq	%rbx, %r15
  5da73a: 74 2a                        	je	0x5da766 <module_init+0x5da746>
  5da73c: 4d 85 ff                     	testq	%r15, %r15
  5da73f: 49 89 9e 28 03 00 00         	movq	%rbx, 0x328(%r14)
  5da746: 74 3b                        	je	0x5da783 <module_init+0x5da763>
  5da748: f0                           	lock
  5da749: 41 ff 4f 08                  	decl	0x8(%r15)
  5da74d: 75 34                        	jne	0x5da783 <module_init+0x5da763>
  5da74f: 49 8b 07                     	movq	(%r15), %rax
  5da752: 4c 89 ff                     	movq	%r15, %rdi
  5da755: ff 10                        	callq	*(%rax)
  5da757: f0                           	lock
  5da758: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da75c: 75 25                        	jne	0x5da783 <module_init+0x5da763>
  5da75e: 49 8b 07                     	movq	(%r15), %rax
  5da761: 4c 89 ff                     	movq	%r15, %rdi
  5da764: eb 1a                        	jmp	0x5da780 <module_init+0x5da760>
  5da766: f0                           	lock
  5da767: ff 4b 08                     	decl	0x8(%rbx)
  5da76a: 75 17                        	jne	0x5da783 <module_init+0x5da763>
  5da76c: 48 8b 03                     	movq	(%rbx), %rax
  5da76f: 48 89 df                     	movq	%rbx, %rdi
  5da772: ff 10                        	callq	*(%rax)
  5da774: f0                           	lock
  5da775: ff 4b 0c                     	decl	0xc(%rbx)
  5da778: 75 09                        	jne	0x5da783 <module_init+0x5da763>
  5da77a: 48 8b 03                     	movq	(%rbx), %rax
  5da77d: 48 89 df                     	movq	%rbx, %rdi
  5da780: ff 50 10                     	callq	*0x10(%rax)
  5da783: f0                           	lock
  5da784: ff 4b 08                     	decl	0x8(%rbx)
  5da787: 75 17                        	jne	0x5da7a0 <module_init+0x5da780>
  5da789: 48 8b 03                     	movq	(%rbx), %rax
  5da78c: 48 89 df                     	movq	%rbx, %rdi
  5da78f: ff 10                        	callq	*(%rax)
  5da791: f0                           	lock
  5da792: ff 4b 0c                     	decl	0xc(%rbx)
  5da795: 75 09                        	jne	0x5da7a0 <module_init+0x5da780>
  5da797: 48 8b 03                     	movq	(%rbx), %rax
  5da79a: 48 89 df                     	movq	%rbx, %rdi
  5da79d: ff 50 10                     	callq	*0x10(%rax)
  5da7a0: 49 8b be 20 03 00 00         	movq	0x320(%r14), %rdi
  5da7a7: 48 8b 07                     	movq	(%rdi), %rax
  5da7aa: ff 50 18                     	callq	*0x18(%rax)
  5da7ad: 84 c0                        	testb	%al, %al
  5da7af: 75 3f                        	jne	0x5da7f0 <module_init+0x5da7d0>
  5da7b1: 49 c7 86 20 03 00 00 00 00 00 00     	movq	$0x0, 0x320(%r14)
  5da7bc: 49 8b 9e 28 03 00 00         	movq	0x328(%r14), %rbx
  5da7c3: 48 85 db                     	testq	%rbx, %rbx
  5da7c6: 74 28                        	je	0x5da7f0 <module_init+0x5da7d0>
  5da7c8: 49 c7 86 28 03 00 00 00 00 00 00     	movq	$0x0, 0x328(%r14)
  5da7d3: f0                           	lock
  5da7d4: ff 4b 08                     	decl	0x8(%rbx)
  5da7d7: 75 17                        	jne	0x5da7f0 <module_init+0x5da7d0>
  5da7d9: 48 8b 03                     	movq	(%rbx), %rax
  5da7dc: 48 89 df                     	movq	%rbx, %rdi
  5da7df: ff 10                        	callq	*(%rax)
  5da7e1: f0                           	lock
  5da7e2: ff 4b 0c                     	decl	0xc(%rbx)
  5da7e5: 75 09                        	jne	0x5da7f0 <module_init+0x5da7d0>
  5da7e7: 48 8b 03                     	movq	(%rbx), %rax
  5da7ea: 48 89 df                     	movq	%rbx, %rdi
  5da7ed: ff 50 10                     	callq	*0x10(%rax)
  5da7f0: 48 8b 85 50 ff ff ff         	movq	-0xb0(%rbp), %rax
  5da7f7: 41 b5 01                     	movb	$0x1, %r13b
  5da7fa: 80 38 00                     	cmpb	$0x0, (%rax)
  5da7fd: 0f 84 1e e8 ff ff            	je	0x5d9021 <module_init+0x5d9001>
  5da803: bf b8 00 00 00               	movl	$0xb8, %edi
  5da808: e8 93 6b 90 00               	callq	0xee13a0 <module_init+0xee1380>
  5da80d: c5 f8 10 0d eb ff 43 04      	vmovups	0x443ffeb(%rip), %xmm1  # 0x4a1a800 <plt_log10+0x4a2b0>
  5da815: 49 89 c7                     	movq	%rax, %r15
  5da818: 48 b8 01 00 00 00 01 00 00 00	movabsq	$0x100000001, %rax      # imm = 0x100000001
  5da822: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  5da826: 49 89 47 08                  	movq	%rax, 0x8(%r15)
  5da82a: 48 8d 05 47 9c 89 05         	leaq	0x5899c47(%rip), %rax   # 0x5e74478 <plt_log10+0x14a3f28>
  5da831: 49 8d 5f 10                  	leaq	0x10(%r15), %rbx
  5da835: 49 89 07                     	movq	%rax, (%r15)
  5da838: 48 8d 05 09 50 89 05         	leaq	0x5895009(%rip), %rax   # 0x5e6f848 <plt_log10+0x149f2f8>
  5da83f: c4 c1 78 11 47 18            	vmovups	%xmm0, 0x18(%r15)
  5da845: 49 89 47 10                  	movq	%rax, 0x10(%r15)
  5da849: 4d 89 77 28                  	movq	%r14, 0x28(%r15)
  5da84d: 41 c7 47 30 ff ff ff ff      	movl	$0xffffffff, 0x30(%r15) # imm = 0xFFFFFFFF
  5da855: c4 c1 78 11 47 38            	vmovups	%xmm0, 0x38(%r15)
  5da85b: 49 c7 47 58 00 00 00 00      	movq	$0x0, 0x58(%r15)
  5da863: c4 c1 78 11 4f 60            	vmovups	%xmm1, 0x60(%r15)
  5da869: 49 c7 47 78 00 00 00 00      	movq	$0x0, 0x78(%r15)
  5da871: 41 c7 87 80 00 00 00 00 00 00 00     	movl	$0x0, 0x80(%r15)
  5da87c: c4 c1 78 11 87 88 00 00 00   	vmovups	%xmm0, 0x88(%r15)
  5da885: 41 c7 87 98 00 00 00 02 00 00 00     	movl	$0x2, 0x98(%r15)
  5da890: c4 c1 78 11 87 9c 00 00 00   	vmovups	%xmm0, 0x9c(%r15)
  5da899: 41 c7 87 ac 00 00 00 00 00 00 00     	movl	$0x0, 0xac(%r15)
  5da8a4: 49 c7 87 b0 00 00 00 02 00 00 00     	movq	$0x2, 0xb0(%r15)
  5da8af: f0                           	lock
  5da8b0: 41 ff 47 08                  	incl	0x8(%r15)
  5da8b4: 49 89 5f 18                  	movq	%rbx, 0x18(%r15)
  5da8b8: 4d 39 7f 20                  	cmpq	%r15, 0x20(%r15)
  5da8bc: 74 1e                        	je	0x5da8dc <module_init+0x5da8bc>
  5da8be: f0                           	lock
  5da8bf: 41 ff 47 0c                  	incl	0xc(%r15)
  5da8c3: 49 8b 7f 20                  	movq	0x20(%r15), %rdi
  5da8c7: 48 85 ff                     	testq	%rdi, %rdi
  5da8ca: 74 0c                        	je	0x5da8d8 <module_init+0x5da8b8>
  5da8cc: f0                           	lock
  5da8cd: ff 4f 0c                     	decl	0xc(%rdi)
  5da8d0: 75 06                        	jne	0x5da8d8 <module_init+0x5da8b8>
  5da8d2: 48 8b 07                     	movq	(%rdi), %rax
  5da8d5: ff 50 10                     	callq	*0x10(%rax)
  5da8d8: 4d 89 7f 20                  	movq	%r15, 0x20(%r15)
  5da8dc: f0                           	lock
  5da8dd: 41 ff 4f 08                  	decl	0x8(%r15)
  5da8e1: 75 18                        	jne	0x5da8fb <module_init+0x5da8db>
  5da8e3: 49 8b 07                     	movq	(%r15), %rax
  5da8e6: 4c 89 ff                     	movq	%r15, %rdi
  5da8e9: ff 10                        	callq	*(%rax)
  5da8eb: f0                           	lock
  5da8ec: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da8f0: 75 09                        	jne	0x5da8fb <module_init+0x5da8db>
  5da8f2: 49 8b 07                     	movq	(%r15), %rax
  5da8f5: 4c 89 ff                     	movq	%r15, %rdi
  5da8f8: ff 50 10                     	callq	*0x10(%rax)
  5da8fb: f0                           	lock
  5da8fc: 41 ff 47 08                  	incl	0x8(%r15)
  5da900: 49 89 9e 30 03 00 00         	movq	%rbx, 0x330(%r14)
  5da907: 4d 8b a6 38 03 00 00         	movq	0x338(%r14), %r12
  5da90e: 4d 39 fc                     	cmpq	%r15, %r12
  5da911: 74 2e                        	je	0x5da941 <module_init+0x5da921>
  5da913: 4d 85 e4                     	testq	%r12, %r12
  5da916: 4d 89 be 38 03 00 00         	movq	%r15, 0x338(%r14)
  5da91d: 74 41                        	je	0x5da960 <module_init+0x5da940>
  5da91f: f0                           	lock
  5da920: 41 ff 4c 24 08               	decl	0x8(%r12)
  5da925: 75 39                        	jne	0x5da960 <module_init+0x5da940>
  5da927: 49 8b 04 24                  	movq	(%r12), %rax
  5da92b: 4c 89 e7                     	movq	%r12, %rdi
  5da92e: ff 10                        	callq	*(%rax)
  5da930: f0                           	lock
  5da931: 41 ff 4c 24 0c               	decl	0xc(%r12)
  5da936: 75 28                        	jne	0x5da960 <module_init+0x5da940>
  5da938: 49 8b 04 24                  	movq	(%r12), %rax
  5da93c: 4c 89 e7                     	movq	%r12, %rdi
  5da93f: eb 1c                        	jmp	0x5da95d <module_init+0x5da93d>
  5da941: f0                           	lock
  5da942: 41 ff 4f 08                  	decl	0x8(%r15)
  5da946: 75 18                        	jne	0x5da960 <module_init+0x5da940>
  5da948: 49 8b 07                     	movq	(%r15), %rax
  5da94b: 4c 89 ff                     	movq	%r15, %rdi
  5da94e: ff 10                        	callq	*(%rax)
  5da950: f0                           	lock
  5da951: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da955: 75 09                        	jne	0x5da960 <module_init+0x5da940>
  5da957: 49 8b 07                     	movq	(%r15), %rax
  5da95a: 4c 89 ff                     	movq	%r15, %rdi
  5da95d: ff 50 10                     	callq	*0x10(%rax)
  5da960: f0                           	lock
  5da961: 41 ff 4f 08                  	decl	0x8(%r15)
  5da965: 75 18                        	jne	0x5da97f <module_init+0x5da95f>
  5da967: 49 8b 07                     	movq	(%r15), %rax
  5da96a: 4c 89 ff                     	movq	%r15, %rdi
  5da96d: ff 10                        	callq	*(%rax)
  5da96f: f0                           	lock
  5da970: 41 ff 4f 0c                  	decl	0xc(%r15)
  5da974: 75 09                        	jne	0x5da97f <module_init+0x5da95f>
  5da976: 49 8b 07                     	movq	(%r15), %rax
  5da979: 4c 89 ff                     	movq	%r15, %rdi
  5da97c: ff 50 10                     	callq	*0x10(%rax)
  5da97f: 48 8d 05 0a 4e 45 06         	leaq	0x6454e0a(%rip), %rax   # 0x6a2f790
  5da986: 4c 8d 05 fb ce 45 06         	leaq	0x645cefb(%rip), %r8    # 0x6a37888
  5da98d: 49 8b 9e 30 03 00 00         	movq	0x330(%r14), %rbx
  5da994: 48 8d 35 45 2a 44 04         	leaq	0x4442a45(%rip), %rsi   # 0x4a1d3e0 <plt_log10+0x4ce90>
  5da99b: 48 8d 15 9e 20 44 04         	leaq	0x444209e(%rip), %rdx   # 0x4a1ca40 <plt_log10+0x4c4f0>
  5da9a2: 48 8d 4d c0                  	leaq	-0x40(%rbp), %rcx
  5da9a6: 48 8b 38                     	movq	(%rax), %rdi
  5da9a9: e8 e2 6c b7 00               	callq	0x1151690 <module_init+0x1151670>
  5da9ae: 4c 8b 25 93 86 f9 05         	movq	0x5f98693(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5da9b5: 31 c9                        	xorl	%ecx, %ecx
  5da9b7: 84 c0                        	testb	%al, %al
  5da9b9: 74 03                        	je	0x5da9be <module_init+0x5da99e>
  5da9bb: 8b 4d c0                     	movl	-0x40(%rbp), %ecx
  5da9be: 89 4b 20                     	movl	%ecx, 0x20(%rbx)
  5da9c1: e9 5b e6 ff ff               	jmp	0x5d9021 <module_init+0x5d9001>
  5da9c6: e8 e5 09 3f 04               	callq	0x49cb3b0 <plt___stack_chk_fail>
  5da9cb: 0f 0b                        	ud2
  5da9cd: 90                           	nop
  5da9ce: 90                           	nop
  5da9cf: 90                           	nop
  5da9d0: 55                           	pushq	%rbp
  5da9d1: 48 89 e5                     	movq	%rsp, %rbp
  5da9d4: 41 57                        	pushq	%r15
  5da9d6: 41 56                        	pushq	%r14
  5da9d8: 41 54                        	pushq	%r12
  5da9da: 53                           	pushq	%rbx
  5da9db: 48 83 ec 20                  	subq	$0x20, %rsp
  5da9df: 4c 8b 25 62 86 f9 05         	movq	0x5f98662(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
  5da9e6: 48 89 fb                     	movq	%rdi, %rbx
  5da9e9: bf 2c 00 00 00               	movl	$0x2c, %edi
  5da9ee: 49 8b 04 24                  	movq	(%r12), %rax
  5da9f2: 48 89 45 d8                  	movq	%rax, -0x28(%rbp)
  5da9f6: e8 e5 0a 3f 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5da9fb: 41 89 c6                     	movl	%eax, %r14d
  5da9fe: 85 c0                        	testl	%eax, %eax
  5daa00: 0f 85 dd 00 00 00            	jne	0x5daae3 <module_init+0x5daac3>
  5daa06: 48 8b 05 eb 89 f9 05         	movq	0x5f989eb(%rip), %rax   # 0x65733f8 <plt_log10+0x1ba2ea8>
  5daa0d: 48 8b 0d dc 89 f9 05         	movq	0x5f989dc(%rip), %rcx   # 0x65733f0 <plt_log10+0x1ba2ea0>
  5daa14: 4c 8d 05 6d ce 45 06         	leaq	0x645ce6d(%rip), %r8    # 0x6a37888
  5daa1b: 48 8d 35 fe 30 44 04         	leaq	0x44430fe(%rip), %rsi   # 0x4a1db20 <plt_log10+0x4d5d0>
  5daa22: 48 8d 15 7f 29 44 04         	leaq	0x444297f(%rip), %rdx   # 0x4a1d3a8 <plt_log10+0x4ce58>
  5daa29: 48 89 83 98 02 00 00         	movq	%rax, 0x298(%rbx)
  5daa30: 48 8d 05 59 4d 45 06         	leaq	0x6454d59(%rip), %rax   # 0x6a2f790
  5daa37: 48 89 8b a0 02 00 00         	movq	%rcx, 0x2a0(%rbx)
  5daa3e: 48 8d 4d d4                  	leaq	-0x2c(%rbp), %rcx
  5daa42: 48 8b 38                     	movq	(%rax), %rdi
  5daa45: e8 46 6c b7 00               	callq	0x1151690 <module_init+0x1151670>
  5daa4a: 84 c0                        	testb	%al, %al
  5daa4c: 74 05                        	je	0x5daa53 <module_init+0x5daa33>
  5daa4e: 8b 45 d4                     	movl	-0x2c(%rbp), %eax
  5daa51: eb 09                        	jmp	0x5daa5c <module_init+0x5daa3c>
  5daa53: 31 c0                        	xorl	%eax, %eax
  5daa55: c7 45 d4 00 00 00 00         	movl	$0x0, -0x2c(%rbp)
  5daa5c: 48 89 df                     	movq	%rbx, %rdi
  5daa5f: 89 83 3c 03 00 00            	movl	%eax, 0x33c(%rbx)
  5daa65: e8 76 47 01 00               	callq	0x5ef1e0 <module_init+0x5ef1c0>
  5daa6a: 48 8d 7d d0                  	leaq	-0x30(%rbp), %rdi
  5daa6e: e8 6d 22 3f 04               	callq	0x49ccce0 <plt_sceUserServiceGetInitialUser>
  5daa73: 85 c0                        	testl	%eax, %eax
  5daa75: 74 58                        	je	0x5daacf <module_init+0x5daaaf>
  5daa77: 4c 8d 7d c0                  	leaq	-0x40(%rbp), %r15
  5daa7b: 89 c2                        	movl	%eax, %edx
  5daa7d: 48 8d 35 da 1f 44 04         	leaq	0x4441fda(%rip), %rsi   # 0x4a1ca5e <plt_log10+0x4c50e>
  5daa84: 31 c0                        	xorl	%eax, %eax
  5daa86: 4c 89 ff                     	movq	%r15, %rdi
  5daa89: e8 f2 85 a3 00               	callq	0x1013080 <module_init+0x1013060>
  5daa8e: 83 7d c8 00                  	cmpl	$0x0, -0x38(%rbp)
  5daa92: 74 06                        	je	0x5daa9a <module_init+0x5daa7a>
  5daa94: 4c 8b 45 c0                  	movq	-0x40(%rbp), %r8
  5daa98: eb 07                        	jmp	0x5daaa1 <module_init+0x5daa81>
  5daa9a: 4c 8d 05 91 1e 44 04         	leaq	0x4441e91(%rip), %r8    # 0x4a1c932 <plt_log10+0x4c3e2>
  5daaa1: 48 8d 3d 93 59 44 04         	leaq	0x4445993(%rip), %rdi   # 0x4a2043b <plt_log10+0x4feeb>
  5daaa8: 48 8d 15 f3 1e 44 04         	leaq	0x4441ef3(%rip), %rdx   # 0x4a1c9a2 <plt_log10+0x4c452>
  5daaaf: 48 8d 0d f6 1e 44 04         	leaq	0x4441ef6(%rip), %rcx   # 0x4a1c9ac <plt_log10+0x4c45c>
  5daab6: be bf 02 00 00               	movl	$0x2bf, %esi            # imm = 0x2BF
  5daabb: 31 c0                        	xorl	%eax, %eax
  5daabd: e8 de 08 b6 00               	callq	0x113b3a0 <module_init+0x113b380>
  5daac2: 4c 89 ff                     	movq	%r15, %rdi
  5daac5: e8 76 44 a3 ff               	callq	0xef40 <module_init+0xef20>
  5daaca: e8 a1 08 b6 00               	callq	0x113b370 <module_init+0x113b350>
  5daacf: 8b bb 3c 03 00 00            	movl	0x33c(%rbx), %edi
  5daad5: 8b 75 d0                     	movl	-0x30(%rbp), %esi
  5daad8: e8 b3 2c 3f 04               	callq	0x49cd790 <plt_sceNpTusCreateNpTitleCtxA>
  5daadd: 89 83 40 03 00 00            	movl	%eax, 0x340(%rbx)
  5daae3: 45 85 f6                     	testl	%r14d, %r14d
  5daae6: 49 8b 0c 24                  	movq	(%r12), %rcx
  5daaea: 0f 94 c0                     	sete	%al
  5daaed: 48 3b 4d d8                  	cmpq	-0x28(%rbp), %rcx
  5daaf1: 75 0d                        	jne	0x5dab00 <module_init+0x5daae0>
  5daaf3: 48 83 c4 20                  	addq	$0x20, %rsp
  5daaf7: 5b                           	popq	%rbx
  5daaf8: 41 5c                        	popq	%r12
  5daafa: 41 5e                        	popq	%r14
  5daafc: 41 5f                        	popq	%r15
  5daafe: 5d                           	popq	%rbp
  5daaff: c3                           	retq
  5dab00: e8 ab 08 3f 04               	callq	0x49cb3b0 <plt___stack_chk_fail>
  5dab05: 0f 0b                        	ud2
  5dab07: 90                           	nop
  5dab08: 90                           	nop
  5dab09: 90                           	nop
  5dab0a: 90                           	nop
  5dab0b: 90                           	nop
  5dab0c: 90                           	nop
  5dab0d: 90                           	nop
  5dab0e: 90                           	nop
  5dab0f: 90                           	nop
  5dab10: 55                           	pushq	%rbp
  5dab11: 48 89 e5                     	movq	%rsp, %rbp
  5dab14: 41 57                        	pushq	%r15
  5dab16: 41 56                        	pushq	%r14
  5dab18: 53                           	pushq	%rbx
  5dab19: 48 83 ec 18                  	subq	$0x18, %rsp
  5dab1d: 4c 8b 3d 24 85 f9 05         	movq	0x5f98524(%rip), %r15   # 0x6573048 <plt_log10+0x1ba2af8>
  5dab24: 48 89 fb                     	movq	%rdi, %rbx
  5dab27: 49 8b 07                     	movq	(%r15), %rax
  5dab2a: 48 89 45 e0                  	movq	%rax, -0x20(%rbp)
  5dab2e: e8 ad a2 f1 ff               	callq	0x4f4de0 <module_init+0x4f4dc0>
  5dab33: 48 8d 05 66 02 49 06         	leaq	0x6490266(%rip), %rax   # 0x6a6ada0
  5dab3a: 4c 8b 30                     	movq	(%rax), %r14
  5dab3d: 4d 85 f6                     	testq	%r14, %r14
  5dab40: 74 4b                        	je	0x5dab8d <module_init+0x5dab6d>
  5dab42: 48 8b b3 90 04 00 00         	movq	0x490(%rbx), %rsi
  5dab49: 48 85 f6                     	testq	%rsi, %rsi
  5dab4c: 74 0c                        	je	0x5dab5a <module_init+0x5dab3a>
  5dab4e: 49 8d be 68 01 00 00         	leaq	0x168(%r14), %rdi
  5dab55: e8 26 5a a9 ff               	callq	0x70580 <module_init+0x70560>
  5dab5a: 48 8b b3 98 04 00 00         	movq	0x498(%rbx), %rsi
  5dab61: 48 85 f6                     	testq	%rsi, %rsi
  5dab64: 74 0c                        	je	0x5dab72 <module_init+0x5dab52>
  5dab66: 49 8d be 80 01 00 00         	leaq	0x180(%r14), %rdi
  5dab6d: e8 0e 5a a9 ff               	callq	0x70580 <module_init+0x70560>
  5dab72: 48 8b b3 a0 04 00 00         	movq	0x4a0(%rbx), %rsi
  5dab79: 48 85 f6                     	testq	%rsi, %rsi
  5dab7c: 74 0f                        	je	0x5dab8d <module_init+0x5dab6d>
  5dab7e: 49 81 c6 98 01 00 00         	addq	$0x198, %r14            # imm = 0x198
  5dab85: 4c 89 f7                     	movq	%r14, %rdi
  5dab88: e8 f3 59 a9 ff               	callq	0x70580 <module_init+0x70560>
  5dab8d: 48 89 df                     	movq	%rbx, %rdi
  5dab90: e8 eb d9 ff ff               	callq	0x5d8580 <module_init+0x5d8560>
  5dab95: 48 8b bb 48 03 00 00         	movq	0x348(%rbx), %rdi
  5dab9c: 48 85 ff                     	testq	%rdi, %rdi
  5dab9f: 74 11                        	je	0x5dabb2 <module_init+0x5dab92>
  5daba1: 48 8b 07                     	movq	(%rdi), %rax
  5daba4: ff 50 28                     	callq	*0x28(%rax)
  5daba7: 48 c7 83 48 03 00 00 00 00 00 00     	movq	$0x0, 0x348(%rbx)
  5dabb2: 48 8b bb 40 03 00 00         	movq	0x340(%rbx), %rdi
  5dabb9: 48 85 ff                     	testq	%rdi, %rdi
  5dabbc: 74 11                        	je	0x5dabcf <module_init+0x5dabaf>
  5dabbe: 48 8b 07                     	movq	(%rdi), %rax
  5dabc1: ff 50 30                     	callq	*0x30(%rax)
  5dabc4: 48 c7 83 40 03 00 00 00 00 00 00     	movq	$0x0, 0x340(%rbx)
  5dabcf: 48 8b bb 20 03 00 00         	movq	0x320(%rbx), %rdi
  5dabd6: 48 85 ff                     	testq	%rdi, %rdi
  5dabd9: 74 06                        	je	0x5dabe1 <module_init+0x5dabc1>
  5dabdb: 48 8b 07                     	movq	(%rdi), %rax
  5dabde: ff 50 10                     	callq	*0x10(%rax)
  5dabe1: 48 8b bb e0 02 00 00         	movq	0x2e0(%rbx), %rdi
  5dabe8: 48 85 ff                     	testq	%rdi, %rdi
  5dabeb: 74 24                        	je	0x5dac11 <module_init+0x5dabf1>
  5dabed: 48 81 c7 a8 00 00 00         	addq	$0xa8, %rdi
  5dabf4: e8 c7 63 00 00               	callq	0x5e0fc0 <module_init+0x5e0fa0>
  5dabf9: bf 2c 00 00 00               	movl	$0x2c, %edi
  5dabfe: e8 7d 2d 3f 04               	callq	0x49cd980 <plt_sceSysmoduleIsLoaded>
  5dac03: 85 c0                        	testl	%eax, %eax
  5dac05: 74 0a                        	je	0x5dac11 <module_init+0x5dabf1>
  5dac07: bf 2c 00 00 00               	movl	$0x2c, %edi
  5dac0c: e8 7f 2d 3f 04               	callq	0x49cd990 <plt_sceSysmoduleUnloadModule>
  5dac11: 48 8b bb b0 02 00 00         	movq	0x2b0(%rbx), %rdi
  5dac18: 48 85 ff                     	testq	%rdi, %rdi
  5dac1b: 74 16                        	je	0x5dac33 <module_init+0x5dac13>
  5dac1d: 48 81 c7 58 01 00 00         	addq	$0x158, %rdi            # imm = 0x158
  5dac24: e8 97 63 00 00               	callq	0x5e0fc0 <module_init+0x5e0fa0>
  5dac29: bf 2c 00 00 00               	movl	$0x2c, %edi
  5dac2e: e8 5d 2d 3f 04               	callq	0x49cd990 <plt_sceSysmoduleUnloadModule>
  5dac33: 48 83 bb 20 03 00 00 00      	cmpq	$0x0, 0x320(%rbx)
  5dac3b: 74 63                        	je	0x5daca0 <module_init+0x5dac80>
  5dac3d: 48 8b 83 28 03 00 00         	movq	0x328(%rbx), %rax
  5dac44: 48 85 c0                     	testq	%rax, %rax
  5dac47: 74 4c                        	je	0x5dac95 <module_init+0x5dac75>
  5dac49: 0f ae f0                     	mfence
  5dac4c: 8b 40 08                     	movl	0x8(%rax), %eax
  5dac4f: 0f ae f0                     	mfence
  5dac52: 4c 8b b3 28 03 00 00         	movq	0x328(%rbx), %r14
  5dac59: 48 c7 83 20 03 00 00 00 00 00 00     	movq	$0x0, 0x320(%rbx)
  5dac64: 4d 85 f6                     	testq	%r14, %r14
  5dac67: 74 37                        	je	0x5daca0 <module_init+0x5dac80>
  5dac69: 48 c7 83 28 03 00 00 00 00 00 00     	movq	$0x0, 0x328(%rbx)
  5dac74: f0                           	lock
  5dac75: 41 ff 4e 08                  	decl	0x8(%r14)
  5dac79: 75 25                        	jne	0x5daca0 <module_init+0x5dac80>
  5dac7b: 49 8b 06                     	movq	(%r14), %rax
  5dac7e: 4c 89 f7                     	movq	%r14, %rdi
  5dac81: ff 10                        	callq	*(%rax)
  5dac83: f0                           	lock
  5dac84: 41 ff 4e 0c                  	decl	0xc(%r14)
  5dac88: 75 16                        	jne	0x5daca0 <module_init+0x5dac80>
  5dac8a: 49 8b 06                     	movq	(%r14), %rax
  5dac8d: 4c 89 f7                     	movq	%r14, %rdi
  5dac90: ff 50 10                     	callq	*0x10(%rax)
  5dac93: eb 0b                        	jmp	0x5daca0 <module_init+0x5dac80>
  5dac95: 48 c7 83 20 03 00 00 00 00 00 00     	movq	$0x0, 0x320(%rbx)
  5daca0: 48 83 bb e0 02 00 00 00      	cmpq	$0x0, 0x2e0(%rbx)
  5daca8: 74 63                        	je	0x5dad0d <module_init+0x5daced>
  5dacaa: 48 8b 83 e8 02 00 00         	movq	0x2e8(%rbx), %rax
  5dacb1: 48 85 c0                     	testq	%rax, %rax
  5dacb4: 74 4c                        	je	0x5dad02 <module_init+0x5dace2>
  5dacb6: 0f ae f0                     	mfence
  5dacb9: 8b 40 08                     	movl	0x8(%rax), %eax
  5dacbc: 0f ae f0                     	mfence
  5dacbf: 4c 8b b3 e8 02 00 00         	movq	0x2e8(%rbx), %r14
  5dacc6: 48 c7 83 e0 02 00 00 00 00 00 00     	movq	$0x0, 0x2e0(%rbx)
  5dacd1: 4d 85 f6                     	testq	%r14, %r14
  5dacd4: 74 37                        	je	0x5dad0d <module_init+0x5daced>
  5dacd6: 48 c7 83 e8 02 00 00 00 00 00 00     	movq	$0x0, 0x2e8(%rbx)
  5dace1: f0                           	lock
  5dace2: 41 ff 4e 08                  	decl	0x8(%r14)
  5dace6: 75 25                        	jne	0x5dad0d <module_init+0x5daced>
  5dace8: 49 8b 06                     	movq	(%r14), %rax
  5daceb: 4c 89 f7                     	movq	%r14, %rdi
  5dacee: ff 10                        	callq	*(%rax)
  5dacf0: f0                           	lock
  5dacf1: 41 ff 4e 0c                  	decl	0xc(%r14)
  5dacf5: 75 16                        	jne	0x5dad0d <module_init+0x5daced>
  5dacf7: 49 8b 06                     	movq	(%r14), %rax
  5dacfa: 4c 89 f7                     	movq	%r14, %rdi
  5dacfd: ff 50 10                     	callq	*0x10(%rax)
  5dad00: eb 0b                        	jmp	0x5dad0d <module_init+0x5daced>
  5dad02: 48 c7 83 e0 02 00 00 00 00 00 00     	movq	$0x0, 0x2e0(%rbx)
  5dad0d: 48 83 bb b0 02 00 00 00      	cmpq	$0x0, 0x2b0(%rbx)
  5dad15: 74 63                        	je	0x5dad7a <module_init+0x5dad5a>
  5dad17: 48 8b 83 b8 02 00 00         	movq	0x2b8(%rbx), %rax
  5dad1e: 48 85 c0                     	testq	%rax, %rax
  5dad21: 74 4c                        	je	0x5dad6f <module_init+0x5dad4f>
  5dad23: 0f ae f0                     	mfence
  5dad26: 8b 40 08                     	movl	0x8(%rax), %eax
  5dad29: 0f ae f0                     	mfence
  5dad2c: 4c 8b b3 b8 02 00 00         	movq	0x2b8(%rbx), %r14
  5dad33: 48 c7 83 b0 02 00 00 00 00 00 00     	movq	$0x0, 0x2b0(%rbx)
  5dad3e: 4d 85 f6                     	testq	%r14, %r14
  5dad41: 74 37                        	je	0x5dad7a <module_init+0x5dad5a>
  5dad43: 48 c7 83 b8 02 00 00 00 00 00 00     	movq	$0x0, 0x2b8(%rbx)
  5dad4e: f0                           	lock
  5dad4f: 41 ff 4e 08                  	decl	0x8(%r14)
  5dad53: 75 25                        	jne	0x5dad7a <module_init+0x5dad5a>
  5dad55: 49 8b 06                     	movq	(%r14), %rax
  5dad58: 4c 89 f7                     	movq	%r14, %rdi
  5dad5b: ff 10                        	callq	*(%rax)
  5dad5d: f0                           	lock
  5dad5e: 41 ff 4e 0c                  	decl	0xc(%r14)
  5dad62: 75 16                        	jne	0x5dad7a <module_init+0x5dad5a>
  5dad64: 49 8b 06                     	movq	(%r14), %rax
  5dad67: 4c 89 f7                     	movq	%r14, %rdi
  5dad6a: ff 50 10                     	callq	*0x10(%rax)
  5dad6d: eb 0b                        	jmp	0x5dad7a <module_init+0x5dad5a>
  5dad6f: 48 c7 83 b0 02 00 00 00 00 00 00     	movq	$0x0, 0x2b0(%rbx)
  5dad7a: 48 83 bb 10 03 00 00 00      	cmpq	$0x0, 0x310(%rbx)
  5dad82: 74 63                        	je	0x5dade7 <module_init+0x5dadc7>
  5dad84: 48 8b 83 18 03 00 00         	movq	0x318(%rbx), %rax
  5dad8b: 48 85 c0                     	testq	%rax, %rax
  5dad8e: 74 4c                        	je	0x5daddc <module_init+0x5dadbc>
  5dad90: 0f ae f0                     	mfence
  5dad93: 8b 40 08                     	movl	0x8(%rax), %eax
  5dad96: 0f ae f0                     	mfence
  5dad99: 4c 8b b3 18 03 00 00         	movq	0x318(%rbx), %r14
  5dada0: 48 c7 83 10 03 00 00 00 00 00 00     	movq	$0x0, 0x310(%rbx)
  5dadab: 4d 85 f6                     	testq	%r14, %r14
  5dadae: 74 37                        	je	0x5dade7 <module_init+0x5dadc7>
  5dadb0: 48 c7 83 18 03 00 00 00 00 00 00     	movq	$0x0, 0x318(%rbx)
  5dadbb: f0                           	lock
  5dadbc: 41 ff 4e 08                  	decl	0x8(%r14)
  5dadc0: 75 25                        	jne	0x5dade7 <module_init+0x5dadc7>
  5dadc2: 49 8b 06                     	movq	(%r14), %rax
  5dadc5: 4c 89 f7                     	movq	%r14, %rdi
  5dadc8: ff 10                        	callq	*(%rax)
  5dadca: f0                           	lock
  5dadcb: 41 ff 4e 0c                  	decl	0xc(%r14)
  5dadcf: 75 16                        	jne	0x5dade7 <module_init+0x5dadc7>
  5dadd1: 49 8b 06                     	movq	(%r14), %rax
  5dadd4: 4c 89 f7                     	movq	%r14, %rdi
  5dadd7: ff 50 10                     	callq	*0x10(%rax)
  5dadda: eb 0b                        	jmp	0x5dade7 <module_init+0x5dadc7>
  5daddc: 48 c7 83 10 03 00 00 00 00 00 00     	movq	$0x0, 0x310(%rbx)
  5dade7: 48 83 bb 00 03 00 00 00      	cmpq	$0x0, 0x300(%rbx)
  5dadef: 74 63                        	je	0x5dae54 <module_init+0x5dae34>
  5dadf1: 48 8b 83 08 03 00 00         	movq	0x308(%rbx), %rax
  5dadf8: 48 85 c0                     	testq	%rax, %rax
  5dadfb: 74 4c                        	je	0x5dae49 <module_init+0x5dae29>
  5dadfd: 0f ae f0                     	mfence
  5dae00: 8b 40 08                     	movl	0x8(%rax), %eax
  5dae03: 0f ae f0                     	mfence
  5dae06: 4c 8b b3 08 03 00 00         	movq	0x308(%rbx), %r14
  5dae0d: 48 c7 83 00 03 00 00 00 00 00 00     	movq	$0x0, 0x300(%rbx)
  5dae18: 4d 85 f6                     	testq	%r14, %r14
  5dae1b: 74 37                        	je	0x5dae54 <module_init+0x5dae34>
  5dae1d: 48 c7 83 08 03 00 00 00 00 00 00     	movq	$0x0, 0x308(%rbx)
  5dae28: f0                           	lock
  5dae29: 41 ff 4e 08                  	decl	0x8(%r14)
  5dae2d: 75 25                        	jne	0x5dae54 <module_init+0x5dae34>
  5dae2f: 49 8b 06                     	movq	(%r14), %rax
  5dae32: 4c 89 f7                     	movq	%r14, %rdi
  5dae35: ff 10                        	callq	*(%rax)
  5dae37: f0                           	lock
  5dae38: 41 ff 4e 0c                  	decl	0xc(%r14)
  5dae3c: 75 16                        	jne	0x5dae54 <module_init+0x5dae34>
  5dae3e: 49 8b 06                     	movq	(%r14), %rax
  5dae41: 4c 89 f7                     	movq	%r14, %rdi
  5dae44: ff 50 10                     	callq	*0x10(%rax)
  5dae47: eb 0b                        	jmp	0x5dae54 <module_init+0x5dae34>
  5dae49: 48 c7 83 00 03 00 00 00 00 00 00     	movq	$0x0, 0x300(%rbx)
  5dae54: 48 83 bb f0 02 00 00 00      	cmpq	$0x0, 0x2f0(%rbx)
  5dae5c: 74 63                        	je	0x5daec1 <module_init+0x5daea1>
  5dae5e: 48 8b 83 f8 02 00 00         	movq	0x2f8(%rbx), %rax
  5dae65: 48 85 c0                     	testq	%rax, %rax
  5dae68: 74 4c                        	je	0x5daeb6 <module_init+0x5dae96>
  5dae6a: 0f ae f0                     	mfence
  5dae6d: 8b 40 08                     	movl	0x8(%rax), %eax
  5dae70: 0f ae f0                     	mfence
  5dae73: 4c 8b b3 f8 02 00 00         	movq	0x2f8(%rbx), %r14
  5dae7a: 48 c7 83 f0 02 00 00 00 00 00 00     	movq	$0x0, 0x2f0(%rbx)
  5dae85: 4d 85 f6                     	testq	%r14, %r14
  5dae88: 74 37                        	je	0x5daec1 <module_init+0x5daea1>
  5dae8a: 48 c7 83 f8 02 00 00 00 00 00 00     	movq	$0x0, 0x2f8(%rbx)
  5dae95: f0                           	lock
  5dae96: 41 ff 4e 08                  	decl	0x8(%r14)
  5dae9a: 75 25                        	jne	0x5daec1 <module_init+0x5daea1>
  5dae9c: 49 8b 06                     	movq	(%r14), %rax
  5dae9f: 4c 89 f7                     	movq	%r14, %rdi
  5daea2: ff 10                        	callq	*(%rax)
  5daea4: f0                           	lock
  5daea5: 41 ff 4e 0c                  	decl	0xc(%r14)
  5daea9: 75 16                        	jne	0x5daec1 <module_init+0x5daea1>
  5daeab: 49 8b 06                     	movq	(%r14), %rax
  5daeae: 4c 89 f7                     	movq	%r14, %rdi
  5daeb1: ff 50 10                     	callq	*0x10(%rax)
  5daeb4: eb 0b                        	jmp	0x5daec1 <module_init+0x5daea1>
  5daeb6: 48 c7 83 f0 02 00 00 00 00 00 00     	movq	$0x0, 0x2f0(%rbx)
  5daec1: 48 83 bb d0 02 00 00 00      	cmpq	$0x0, 0x2d0(%rbx)
  5daec9: 74 63                        	je	0x5daf2e <module_init+0x5daf0e>
  5daecb: 48 8b 83 d8 02 00 00         	movq	0x2d8(%rbx), %rax
  5daed2: 48 85 c0                     	testq	%rax, %rax
  5daed5: 74 4c                        	je	0x5daf23 <module_init+0x5daf03>
  5daed7: 0f ae f0                     	mfence
  5daeda: 8b 40 08                     	movl	0x8(%rax), %eax
  5daedd: 0f ae f0                     	mfence
  5daee0: 4c 8b b3 d8 02 00 00         	movq	0x2d8(%rbx), %r14
  5daee7: 48 c7 83 d0 02 00 00 00 00 00 00     	movq	$0x0, 0x2d0(%rbx)
  5daef2: 4d 85 f6                     	testq	%r14, %r14
  5daef5: 74 37                        	je	0x5daf2e <module_init+0x5daf0e>
  5daef7: 48 c7 83 d8 02 00 00 00 00 00 00     	movq	$0x0, 0x2d8(%rbx)
  5daf02: f0                           	lock
  5daf03: 41 ff 4e 08                  	decl	0x8(%r14)
  5daf07: 75 25                        	jne	0x5daf2e <module_init+0x5daf0e>
  5daf09: 49 8b 06                     	movq	(%r14), %rax
  5daf0c: 4c 89 f7                     	movq	%r14, %rdi
  5daf0f: ff 10                        	callq	*(%rax)
  5daf11: f0                           	lock
  5daf12: 41 ff 4e 0c                  	decl	0xc(%r14)
  5daf16: 75 16                        	jne	0x5daf2e <module_init+0x5daf0e>
  5daf18: 49 8b 06                     	movq	(%r14), %rax
  5daf1b: 4c 89 f7                     	movq	%r14, %rdi
  5daf1e: ff 50 10                     	callq	*0x10(%rax)
  5daf21: eb 0b                        	jmp	0x5daf2e <module_init+0x5daf0e>
  5daf23: 48 c7 83 d0 02 00 00 00 00 00 00     	movq	$0x0, 0x2d0(%rbx)
  5daf2e: 48 83 bb c0 02 00 00 00      	cmpq	$0x0, 0x2c0(%rbx)
  5daf36: 74 63                        	je	0x5daf9b <module_init+0x5daf7b>
  5daf38: 48 8b 83 c8 02 00 00         	movq	0x2c8(%rbx), %rax
  5daf3f: 48 85 c0                     	testq	%rax, %rax
  5daf42: 74 4c                        	je	0x5daf90 <module_init+0x5daf70>
  5daf44: 0f ae f0                     	mfence
  5daf47: 8b 40 08                     	movl	0x8(%rax), %eax
  5daf4a: 0f ae f0                     	mfence
  5daf4d: 4c 8b b3 c8 02 00 00         	movq	0x2c8(%rbx), %r14
  5daf54: 48 c7 83 c0 02 00 00 00 00 00 00     	movq	$0x0, 0x2c0(%rbx)
  5daf5f: 4d 85 f6                     	testq	%r14, %r14
  5daf62: 74 37                        	je	0x5daf9b <module_init+0x5daf7b>
  5daf64: 48 c7 83 c8 02 00 00 00 00 00 00     	movq	$0x0, 0x2c8(%rbx)
  5daf6f: f0                           	lock
  5daf70: 41 ff 4e 08                  	decl	0x8(%r14)
  5daf74: 75 25                        	jne	0x5daf9b <module_init+0x5daf7b>
  5daf76: 49 8b 06                     	movq	(%r14), %rax
  5daf79: 4c 89 f7                     	movq	%r14, %rdi
  5daf7c: ff 10                        	callq	*(%rax)
  5daf7e: f0                           	lock
  5daf7f: 41 ff 4e 0c                  	decl	0xc(%r14)
  5daf83: 75 16                        	jne	0x5daf9b <module_init+0x5daf7b>
  5daf85: 49 8b 06                     	movq	(%r14), %rax
  5daf88: 4c 89 f7                     	movq	%r14, %rdi
  5daf8b: ff 50 10                     	callq	*0x10(%rax)
  5daf8e: eb 0b                        	jmp	0x5daf9b <module_init+0x5daf7b>
  5daf90: 48 c7 83 c0 02 00 00 00 00 00 00     	movq	$0x0, 0x2c0(%rbx)
  5daf9b: 48 83 bb 80 02 00 00 00      	cmpq	$0x0, 0x280(%rbx)
  5dafa3: 74 63                        	je	0x5db008 <module_init+0x5dafe8>
  5dafa5: 48 8b 83 88 02 00 00         	movq	0x288(%rbx), %rax
  5dafac: 48 85 c0                     	testq	%rax, %rax
  5dafaf: 74 4c                        	je	0x5daffd <module_init+0x5dafdd>
  5dafb1: 0f ae f0                     	mfence
  5dafb4: 8b 40 08                     	movl	0x8(%rax), %eax
  5dafb7: 0f ae f0                     	mfence
  5dafba: 4c 8b b3 88 02 00 00         	movq	0x288(%rbx), %r14
  5dafc1: 48 c7 83 80 02 00 00 00 00 00 00     	movq	$0x0, 0x280(%rbx)
  5dafcc: 4d 85 f6                     	testq	%r14, %r14
  5dafcf: 74 37                        	je	0x5db008 <module_init+0x5dafe8>
  5dafd1: 48 c7 83 88 02 00 00 00 00 00 00     	movq	$0x0, 0x288(%rbx)
  5dafdc: f0                           	lock
  5dafdd: 41 ff 4e 08                  	decl	0x8(%r14)
  5dafe1: 75 25                        	jne	0x5db008 <module_init+0x5dafe8>
  5dafe3: 49 8b 06                     	movq	(%r14), %rax
  5dafe6: 4c 89 f7                     	movq	%r14, %rdi
  5dafe9: ff 10                        	callq	*(%rax)
  5dafeb: f0                           	lock
  5dafec: 41 ff 4e 0c                  	decl	0xc(%r14)
  5daff0: 75 16                        	jne	0x5db008 <module_init+0x5dafe8>
  5daff2: 49 8b 06                     	movq	(%r14), %rax
  5daff5: 4c 89 f7                     	movq	%r14, %rdi
  5daff8: ff 50 10                     	callq	*0x10(%rax)
  5daffb: eb 0b                        	jmp	0x5db008 <module_init+0x5dafe8>
  5daffd: 48 c7 83 80 02 00 00 00 00 00 00     	movq	$0x0, 0x280(%rbx)
