
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  59b080: 5a                           	popq	%rdx
  59b081: ff ff                        	<unknown>
  59b083: 80 bb a4 00 00 00 00         	cmpb	$0x0, 0xa4(%rbx)
  59b08a: 0f 84 b9 00 00 00            	je	0x59b149 <module_init+0x59b129>
  59b090: c5 f8 10 05 38 c7 47 04      	vmovups	0x447c738(%rip), %xmm0  # 0x4a177d0 <plt_log10+0x47280>
  59b098: 48 8d 43 78                  	leaq	0x78(%rbx), %rax
  59b09c: c5 f0 57 c9                  	vxorps	%xmm1, %xmm1, %xmm1
  59b0a0: 48 8d bc 24 80 00 00 00      	leaq	0x80(%rsp), %rdi
  59b0a8: 31 d2                        	xorl	%edx, %edx
  59b0aa: c5 f8 11 44 24 20            	vmovups	%xmm0, 0x20(%rsp)
  59b0b0: c5 f8 10 83 a8 00 00 00      	vmovups	0xa8(%rbx), %xmm0
  59b0b8: c5 fc 11 8c 24 b0 00 00 00   	vmovups	%ymm1, 0xb0(%rsp)
  59b0c1: c5 fc 11 8c 24 80 00 00 00   	vmovups	%ymm1, 0x80(%rsp)
  59b0ca: c5 fc 11 8c 24 a0 00 00 00   	vmovups	%ymm1, 0xa0(%rsp)
  59b0d3: 48 89 84 24 80 00 00 00      	movq	%rax, 0x80(%rsp)
  59b0db: 48 8d 44 24 20               	leaq	0x20(%rsp), %rax
  59b0e0: 48 89 84 24 88 00 00 00      	movq	%rax, 0x88(%rsp)
  59b0e8: c5 f8 11 84 24 90 00 00 00   	vmovups	%xmm0, 0x90(%rsp)
  59b0f1: 48 8b 83 98 00 00 00         	movq	0x98(%rbx), %rax
  59b0f8: 48 89 84 24 a0 00 00 00      	movq	%rax, 0xa0(%rsp)
  59b100: c7 84 24 a8 00 00 00 a0 0f 00 00     	movl	$0xfa0, 0xa8(%rsp) # imm = 0xFA0
  59b10b: 8b 73 30                     	movl	0x30(%rbx), %esi
  59b10e: e8 5d 1a 43 04               	callq	0x49ccb70 <plt_sceHmdReprojectionStart2dVr>
  59b113: 85 c0                        	testl	%eax, %eax
  59b115: 0f 89 67 02 00 00            	jns	0x59b382 <module_init+0x59b362>
  59b11b: 41 89 c0                     	movl	%eax, %r8d
  59b11e: 48 8d 3d 78 e5 47 04         	leaq	0x447e578(%rip), %rdi   # 0x4a1969d <plt_log10+0x4914d>
  59b125: 48 8d 15 aa db 47 04         	leaq	0x447dbaa(%rip), %rdx   # 0x4a18cd6 <plt_log10+0x48786>
  59b12c: 48 8d 0d b9 e4 47 04         	leaq	0x447e4b9(%rip), %rcx   # 0x4a195ec <plt_log10+0x4909c>
  59b133: be cc 01 00 00               	movl	$0x1cc, %esi            # imm = 0x1CC
  59b138: 31 c0                        	xorl	%eax, %eax
  59b13a: e8 61 02 ba 00               	callq	0x113b3a0 <module_init+0x113b380>
  59b13f: e8 2c 02 ba 00               	callq	0x113b370 <module_init+0x113b350>
  59b144: e9 39 02 00 00               	jmp	0x59b382 <module_init+0x59b362>
  59b149: c5 fa 10 0d 17 f5 09 06      	vmovss	0x609f517(%rip), %xmm1  # xmm1 = mem[0],zero,zero,zero
                                                                        # 0x663a668
  59b151: c5 f2 58 05 13 f5 09 06      	vaddss	0x609f513(%rip), %xmm1, %xmm0 # 0x663a66c
  59b159: c5 fa 10 15 eb c3 47 04      	vmovss	0x447c3eb(%rip), %xmm2  # xmm2 = mem[0],zero,zero,zero
                                                                        # 0x4a1754c <plt_log10+0x46ffc>
  59b161: c5 fa 10 1d f7 f4 09 06      	vmovss	0x609f4f7(%rip), %xmm3  # xmm3 = mem[0],zero,zero,zero
                                                                        # 0x663a660
  59b169: c7 84 24 04 01 00 00 00 f0 ff 00     	movl	$0xfff000, 0x104(%rsp) # imm = 0xFFF000
  59b174: 4c 8d 73 38                  	leaq	0x38(%rbx), %r14
  59b178: 8b 83 a0 00 00 00            	movl	0xa0(%rbx), %eax
  59b17e: 4c 89 f7                     	movq	%r14, %rdi
  59b181: 83 e0 07                     	andl	$0x7, %eax
  59b184: 8d 0c c0                     	leal	(%rax,%rax,8), %ecx
  59b187: c1 e0 06                     	shll	$0x6, %eax
  59b18a: 09 c1                        	orl	%eax, %ecx
  59b18c: 48 b8 00 00 50 05 00 00 00 40	movabsq	$0x4000000005500000, %rax # imm = 0x4000000005500000
  59b196: c5 ea 5e f0                  	vdivss	%xmm0, %xmm2, %xmm6
  59b19a: c5 fa 10 05 c2 f4 09 06      	vmovss	0x609f4c2(%rip), %xmm0  # xmm0 = mem[0],zero,zero,zero
                                                                        # 0x663a664
  59b1a2: 89 8c 24 00 01 00 00         	movl	%ecx, 0x100(%rsp)
  59b1a9: 48 89 84 24 08 01 00 00      	movq	%rax, 0x108(%rsp)
  59b1b1: 48 8d 43 78                  	leaq	0x78(%rbx), %rax
  59b1b5: c5 fa 58 e3                  	vaddss	%xmm3, %xmm0, %xmm4
  59b1b9: c5 ea 5e e4                  	vdivss	%xmm4, %xmm2, %xmm4
  59b1bd: c5 fa 10 15 8b c3 47 04      	vmovss	0x447c38b(%rip), %xmm2  # xmm2 = mem[0],zero,zero,zero
                                                                        # 0x4a17550 <plt_log10+0x47000>
  59b1c5: c5 ca 59 f9                  	vmulss	%xmm1, %xmm6, %xmm7
  59b1c9: c5 f0 57 c9                  	vxorps	%xmm1, %xmm1, %xmm1
  59b1cd: c5 fc 11 8c 24 e0 00 00 00   	vmovups	%ymm1, 0xe0(%rsp)
  59b1d6: c5 fc 11 8c 24 c0 00 00 00   	vmovups	%ymm1, 0xc0(%rsp)
  59b1df: c5 fc 11 8c 24 a0 00 00 00   	vmovups	%ymm1, 0xa0(%rsp)
  59b1e8: c5 fc 11 8c 24 80 00 00 00   	vmovups	%ymm1, 0x80(%rsp)
  59b1f1: c5 fa 11 74 24 0c            	vmovss	%xmm6, 0xc(%rsp)
  59b1f7: c5 fa 10 8b b8 00 00 00      	vmovss	0xb8(%rbx), %xmm1       # xmm1 = mem[0],zero,zero,zero
  59b1ff: c5 fa 11 7c 24 04            	vmovss	%xmm7, 0x4(%rsp)
  59b205: c5 f2 59 f6                  	vmulss	%xmm6, %xmm1, %xmm6
  59b209: c5 fa 11 64 24 08            	vmovss	%xmm4, 0x8(%rsp)
  59b20f: c5 da 59 e2                  	vmulss	%xmm2, %xmm4, %xmm4
  59b213: c5 da 59 c0                  	vmulss	%xmm0, %xmm4, %xmm0
  59b217: c5 f2 59 ec                  	vmulss	%xmm4, %xmm1, %xmm5
  59b21b: c5 fa 58 c2                  	vaddss	%xmm2, %xmm0, %xmm0
  59b21f: c5 d2 59 db                  	vmulss	%xmm3, %xmm5, %xmm3
  59b223: c5 fa 11 9c 24 a0 00 00 00   	vmovss	%xmm3, 0xa0(%rsp)
  59b22c: c5 f2 59 df                  	vmulss	%xmm7, %xmm1, %xmm3
  59b230: c5 f2 59 c0                  	vmulss	%xmm0, %xmm1, %xmm0
  59b234: c5 fa 11 9c 24 a4 00 00 00   	vmovss	%xmm3, 0xa4(%rsp)
  59b23d: c5 fa 11 ac 24 98 00 00 00   	vmovss	%xmm5, 0x98(%rsp)
  59b246: c5 fa 11 b4 24 9c 00 00 00   	vmovss	%xmm6, 0x9c(%rsp)
  59b24f: c5 fa 11 84 24 b0 00 00 00   	vmovss	%xmm0, 0xb0(%rsp)
  59b258: c5 fa 11 9c 24 b4 00 00 00   	vmovss	%xmm3, 0xb4(%rsp)
  59b261: c5 fa 11 ac 24 a8 00 00 00   	vmovss	%xmm5, 0xa8(%rsp)
  59b26a: c5 fa 11 b4 24 ac 00 00 00   	vmovss	%xmm6, 0xac(%rsp)
  59b273: 48 89 84 24 80 00 00 00      	movq	%rax, 0x80(%rsp)
  59b27b: 48 89 84 24 88 00 00 00      	movq	%rax, 0x88(%rsp)
  59b283: 48 8b 83 98 00 00 00         	movq	0x98(%rbx), %rax
  59b28a: 48 89 84 24 b8 00 00 00      	movq	%rax, 0xb8(%rsp)
  59b292: 48 8d 84 24 00 01 00 00      	leaq	0x100(%rsp), %rax
  59b29a: 48 89 84 24 90 00 00 00      	movq	%rax, 0x90(%rsp)
  59b2a2: c7 84 24 c0 00 00 00 b8 0b 00 00     	movl	$0xbb8, 0xc0(%rsp) # imm = 0xBB8
  59b2ad: c7 84 24 d0 00 00 00 00 00 00 00     	movl	$0x0, 0xd0(%rsp)
  59b2b8: 48 c7 84 24 c8 00 00 00 00 00 00 00  	movq	$0x0, 0xc8(%rsp)
  59b2c4: 48 c7 84 24 d8 00 00 00 05 00 00 00  	movq	$0x5, 0xd8(%rsp)
  59b2d0: e8 4b fb 99 03               	callq	0x3f3ae20 <plt_+0x2b55ca>
  59b2d5: 85 c0                        	testl	%eax, %eax
  59b2d7: 0f 84 bd 00 00 00            	je	0x59b39a <module_init+0x59b37a>
  59b2dd: c5 f8 10 05 eb c4 47 04      	vmovups	0x447c4eb(%rip), %xmm0  # 0x4a177d0 <plt_log10+0x47280>
  59b2e5: c5 fa 10 4c 24 08            	vmovss	0x8(%rsp), %xmm1        # xmm1 = mem[0],zero,zero,zero
  59b2eb: 48 8d 43 58                  	leaq	0x58(%rbx), %rax
  59b2ef: 48 c7 44 24 78 00 00 00 00   	movq	$0x0, 0x78(%rsp)
  59b2f8: 48 8d bc 24 80 00 00 00      	leaq	0x80(%rsp), %rdi
  59b300: 48 8d 4c 24 20               	leaq	0x20(%rsp), %rcx
  59b305: 48 89 de                     	movq	%rbx, %rsi
  59b308: 45 31 c0                     	xorl	%r8d, %r8d
  59b30b: c5 f8 11 44 24 10            	vmovups	%xmm0, 0x10(%rsp)
  59b311: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
  59b315: c5 fc 11 44 24 58            	vmovups	%ymm0, 0x58(%rsp)
  59b31b: c5 f2 59 05 3d f3 09 06      	vmulss	0x609f33d(%rip), %xmm1, %xmm0 # 0x663a660
  59b323: 4c 89 74 24 20               	movq	%r14, 0x20(%rsp)
  59b328: 48 89 44 24 28               	movq	%rax, 0x28(%rsp)
  59b32d: 48 8d 44 24 10               	leaq	0x10(%rsp), %rax
  59b332: 48 89 44 24 30               	movq	%rax, 0x30(%rsp)
  59b337: c5 fa 11 44 24 40            	vmovss	%xmm0, 0x40(%rsp)
  59b33d: c5 fa 10 44 24 04            	vmovss	0x4(%rsp), %xmm0        # xmm0 = mem[0],zero,zero,zero
  59b343: c5 fa 11 44 24 44            	vmovss	%xmm0, 0x44(%rsp)
  59b349: c5 fa 10 44 24 0c            	vmovss	0xc(%rsp), %xmm0        # xmm0 = mem[0],zero,zero,zero
  59b34f: c5 fa 11 4c 24 38            	vmovss	%xmm1, 0x38(%rsp)
  59b355: c5 fa 11 44 24 3c            	vmovss	%xmm0, 0x3c(%rsp)
  59b35b: c5 f8 10 44 24 38            	vmovups	0x38(%rsp), %xmm0
  59b361: c5 f8 11 44 24 48            	vmovups	%xmm0, 0x48(%rsp)
  59b367: 8b 53 30                     	movl	0x30(%rbx), %edx
  59b36a: e8 11 18 43 04               	callq	0x49ccb80 <plt_sceHmdReprojectionStartWithOverlay>
  59b36f: 85 c0                        	testl	%eax, %eax
  59b371: 74 0f                        	je	0x59b382 <module_init+0x59b362>
  59b373: 48 8b 84 24 b8 00 00 00      	movq	0xb8(%rsp), %rax
  59b37b: 48 c7 00 00 00 00 00         	movq	$0x0, (%rax)
  59b382: 49 8b 07                     	movq	(%r15), %rax
  59b385: 48 3b 84 24 10 01 00 00      	cmpq	0x110(%rsp), %rax
  59b38d: 75 26                        	jne	0x59b3b5 <module_init+0x59b395>
  59b38f: 48 8d 65 e8                  	leaq	-0x18(%rbp), %rsp
  59b393: 5b                           	popq	%rbx
  59b394: 41 5e                        	popq	%r14
  59b396: 41 5f                        	popq	%r15
  59b398: 5d                           	popq	%rbp
  59b399: c3                           	retq
  59b39a: 8b 53 30                     	movl	0x30(%rbx), %edx
  59b39d: 48 8d bc 24 80 00 00 00      	leaq	0x80(%rsp), %rdi
  59b3a5: 48 89 de                     	movq	%rbx, %rsi
  59b3a8: 31 c9                        	xorl	%ecx, %ecx
  59b3aa: e8 b1 17 43 04               	callq	0x49ccb60 <plt_sceHmdReprojectionStart>
  59b3af: 85 c0                        	testl	%eax, %eax
  59b3b1: 75 c0                        	jne	0x59b373 <module_init+0x59b353>
  59b3b3: eb cd                        	jmp	0x59b382 <module_init+0x59b362>
  59b3b5: e8 f6 ff 42 04               	callq	0x49cb3b0 <plt___stack_chk_fail>
  59b3ba: 0f 0b                        	ud2
  59b3bc: 90                           	nop
  59b3bd: 90                           	nop
  59b3be: 90                           	nop
  59b3bf: 90                           	nop
  59b3c0: 55                           	pushq	%rbp
  59b3c1: 48 89 e5                     	movq	%rsp, %rbp
  59b3c4: 41 57                        	pushq	%r15
  59b3c6: 41 56                        	pushq	%r14
  59b3c8: 53                           	pushq	%rbx
  59b3c9: 50                           	pushq	%rax
  59b3ca: 48 8d 05 17 d2 46 06         	leaq	0x646d217(%rip), %rax   # 0x6a085e8
  59b3d1: 49 89 f6                     	movq	%rsi, %r14
  59b3d4: 48 89 fb                     	movq	%rdi, %rbx
  59b3d7: 4c 8b 38                     	movq	(%rax), %r15
  59b3da: 4d 85 ff                     	testq	%r15, %r15
  59b3dd: 74 42                        	je	0x59b421 <module_init+0x59b401>
  59b3df: 48 8d 05 f6 d1 46 06         	leaq	0x646d1f6(%rip), %rax   # 0x6a085dc
  59b3e6: 8b 38                        	movl	(%rax), %edi
  59b3e8: 85 ff                        	testl	%edi, %edi
  59b3ea: 74 76                        	je	0x59b462 <module_init+0x59b442>
  59b3ec: e8 7f 00 43 04               	callq	0x49cb470 <plt_scePthreadGetspecific>
  59b3f1: 48 85 c0                     	testq	%rax, %rax
  59b3f4: 74 6c                        	je	0x59b462 <module_init+0x59b442>
  59b3f6: 48 8d 15 f3 d1 46 06         	leaq	0x646d1f3(%rip), %rdx   # 0x6a085f0
  59b3fd: 48 89 c1                     	movq	%rax, %rcx
