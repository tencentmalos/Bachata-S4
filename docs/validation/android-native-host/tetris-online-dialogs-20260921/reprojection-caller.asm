
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  590180: 48 81 c4 d8 00 00 00         	addq	$0xd8, %rsp
  590187: 5b                           	popq	%rbx
  590188: 41 5c                        	popq	%r12
  59018a: 41 5d                        	popq	%r13
  59018c: 41 5e                        	popq	%r14
  59018e: 41 5f                        	popq	%r15
  590190: 5d                           	popq	%rbp
  590191: e9 9a 24 a2 00               	jmp	0xfb2630 <module_init+0xfb2610>
  590196: 41 89 c0                     	movl	%eax, %r8d
  590199: 48 8d 3d fd 94 48 04         	leaq	0x44894fd(%rip), %rdi   # 0x4a1969d <plt_log10+0x4914d>
  5901a0: 48 8d 15 2f 8b 48 04         	leaq	0x4488b2f(%rip), %rdx   # 0x4a18cd6 <plt_log10+0x48786>
  5901a7: 48 8d 0d f2 8d 48 04         	leaq	0x4488df2(%rip), %rcx   # 0x4a18fa0 <plt_log10+0x48a50>
  5901ae: be 46 00 00 00               	movl	$0x46, %esi
  5901b3: 31 c0                        	xorl	%eax, %eax
  5901b5: e8 e6 b1 ba 00               	callq	0x113b3a0 <module_init+0x113b380>
  5901ba: e8 b1 b1 ba 00               	callq	0x113b370 <module_init+0x113b350>
  5901bf: 81 7d 80 00 0f 00 00         	cmpl	$0xf00, -0x80(%rbp)     # imm = 0xF00
  5901c6: 72 16                        	jb	0x5901de <module_init+0x5901be>
  5901c8: 81 7d 84 70 08 00 00         	cmpl	$0x870, -0x7c(%rbp)     # imm = 0x870
  5901cf: 72 0d                        	jb	0x5901de <module_init+0x5901be>
  5901d1: 48 8d 05 b0 4a 09 06         	leaq	0x6094ab0(%rip), %rax   # 0x6624c88
  5901d8: c7 00 01 00 00 00            	movl	$0x1, (%rax)
  5901de: 41 8b 47 1c                  	movl	0x1c(%r15), %eax
  5901e2: 41 8b 4f 20                  	movl	0x20(%r15), %ecx
  5901e6: ba 80 07 00 00               	movl	$0x780, %edx            # imm = 0x780
  5901eb: 3d 80 07 00 00               	cmpl	$0x780, %eax            # imm = 0x780
  5901f0: 0f 4c d0                     	cmovll	%eax, %edx
  5901f3: 81 f9 38 04 00 00            	cmpl	$0x438, %ecx            # imm = 0x438
  5901f9: 41 89 57 1c                  	movl	%edx, 0x1c(%r15)
  5901fd: ba 38 04 00 00               	movl	$0x438, %edx            # imm = 0x438
  590202: 0f 4c d1                     	cmovll	%ecx, %edx
  590205: 41 89 57 20                  	movl	%edx, 0x20(%r15)
  590209: e8 22 c9 43 04               	callq	0x49ccb30 <plt_sceHmdReprojectionQueryOnionBuffSize>
  59020e: 48 89 c3                     	movq	%rax, %rbx
  590211: e8 0a c9 43 04               	callq	0x49ccb20 <plt_sceHmdReprojectionQueryOnionBuffAlign>
  590216: 48 8d bd 48 ff ff ff         	leaq	-0xb8(%rbp), %rdi
  59021d: 89 de                        	movl	%ebx, %esi
  59021f: 89 c2                        	movl	%eax, %edx
  590221: b9 01 00 00 00               	movl	$0x1, %ecx
  590226: e8 05 bf a5 00               	callq	0xfec130 <module_init+0xfec110>
  59022b: c5 f8 10 85 48 ff ff ff      	vmovups	-0xb8(%rbp), %xmm0
  590233: c5 f8 10 8d 54 ff ff ff      	vmovups	-0xac(%rbp), %xmm1
  59023b: c4 c1 78 11 8f 24 01 00 00   	vmovups	%xmm1, 0x124(%r15)
  590244: c4 c1 78 11 87 18 01 00 00   	vmovups	%xmm0, 0x118(%r15)
  59024d: e8 be c8 43 04               	callq	0x49ccb10 <plt_sceHmdReprojectionQueryGarlicBuffSize>
  590252: 48 89 c3                     	movq	%rax, %rbx
  590255: e8 a6 c8 43 04               	callq	0x49ccb00 <plt_sceHmdReprojectionQueryGarlicBuffAlign>
  59025a: 48 8d bd 48 ff ff ff         	leaq	-0xb8(%rbp), %rdi
  590261: 89 de                        	movl	%ebx, %esi
  590263: 89 c2                        	movl	%eax, %edx
  590265: 31 c9                        	xorl	%ecx, %ecx
  590267: e8 c4 be a5 00               	callq	0xfec130 <module_init+0xfec110>
  59026c: c5 f8 10 85 48 ff ff ff      	vmovups	-0xb8(%rbp), %xmm0
  590274: c5 f8 10 8d 54 ff ff ff      	vmovups	-0xac(%rbp), %xmm1
  59027c: 48 8d bd 48 ff ff ff         	leaq	-0xb8(%rbp), %rdi
  590283: be 02 00 00 00               	movl	$0x2, %esi
  590288: 31 d2                        	xorl	%edx, %edx
  59028a: c4 c1 78 11 8f 44 01 00 00   	vmovups	%xmm1, 0x144(%r15)
  590293: c4 c1 78 11 87 38 01 00 00   	vmovups	%xmm0, 0x138(%r15)
  59029c: 49 8b 87 18 01 00 00         	movq	0x118(%r15), %rax
  5902a3: 48 89 85 48 ff ff ff         	movq	%rax, -0xb8(%rbp)
  5902aa: 49 8b 87 38 01 00 00         	movq	0x138(%r15), %rax
  5902b1: 48 89 85 50 ff ff ff         	movq	%rax, -0xb0(%rbp)
  5902b8: 48 b8 03 00 00 00 03 00 00 00	movabsq	$0x300000003, %rax      # imm = 0x300000003
  5902c2: c7 85 58 ff ff ff 00 01 00 00	movl	$0x100, -0xa8(%rbp)     # imm = 0x100
  5902cc: 48 c7 85 60 ff ff ff 3f 00 00 00     	movq	$0x3f, -0xa0(%rbp)
  5902d7: 48 89 85 68 ff ff ff         	movq	%rax, -0x98(%rbp)
  5902de: 48 c7 85 70 ff ff ff 00 00 00 00     	movq	$0x0, -0x90(%rbp)
  5902e9: c7 85 78 ff ff ff 00 00 00 00	movl	$0x0, -0x88(%rbp)
  5902f3: e8 f8 c7 43 04               	callq	0x49ccaf0 <plt_sceHmdReprojectionInitialize>
  5902f8: 85 c0                        	testl	%eax, %eax
  5902fa: 79 29                        	jns	0x590325 <module_init+0x590305>
  5902fc: 41 89 c0                     	movl	%eax, %r8d
  5902ff: 48 8d 3d 97 93 48 04         	leaq	0x4489397(%rip), %rdi   # 0x4a1969d <plt_log10+0x4914d>
  590306: 48 8d 15 c9 89 48 04         	leaq	0x44889c9(%rip), %rdx   # 0x4a18cd6 <plt_log10+0x48786>
  59030d: 48 8d 0d fc 8c 48 04         	leaq	0x4488cfc(%rip), %rcx   # 0x4a19010 <plt_log10+0x48ac0>
  590314: be 68 00 00 00               	movl	$0x68, %esi
  590319: 31 c0                        	xorl	%eax, %eax
  59031b: e8 80 b0 ba 00               	callq	0x113b3a0 <module_init+0x113b380>
  590320: e8 4b b0 ba 00               	callq	0x113b370 <module_init+0x113b350>
  590325: c4 c1 7b 12 47 1c            	vmovddup	0x1c(%r15), %xmm0       # xmm0 = mem[0,0]
  59032b: c5 f8 10 15 6d 73 48 04      	vmovups	0x448736d(%rip), %xmm2  # 0x4a176a0 <plt_log10+0x47150>
  590333: c5 f8 10 0d 75 73 48 04      	vmovups	0x4487375(%rip), %xmm1  # 0x4a176b0 <plt_log10+0x47160>
  59033b: 48 b8 ff 00 00 00 07 00 00 00	movabsq	$0x7000000ff, %rax      # imm = 0x7000000FF
