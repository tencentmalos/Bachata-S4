
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  5c3c40: 18 02                        	sbbb	%al, (%rdx)
  5c3c42: 00 00                        	addb	%al, (%rax)
  5c3c44: 00 c5                        	addb	%al, %ch
  5c3c46: f8                           	clc
  5c3c47: 11 47 1c                     	adcl	%eax, 0x1c(%rdi)
  5c3c4a: c7 47 2c 00 00 00 00         	movl	$0x0, 0x2c(%rdi)
  5c3c51: 48 c7 47 30 02 00 00 00      	movq	$0x2, 0x30(%rdi)
  5c3c59: 48 89 07                     	movq	%rax, (%rdi)
  5c3c5c: 48 89 77 38                  	movq	%rsi, 0x38(%rdi)
  5c3c60: c7 47 40 00 00 00 00         	movl	$0x0, 0x40(%rdi)
  5c3c67: bf 1e 00 00 00               	movl	$0x1e, %edi
  5c3c6c: e8 6f 78 40 04               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
  5c3c71: 85 c0                        	testl	%eax, %eax
  5c3c73: 74 58                        	je	0x5c3ccd <module_init+0x5c3cad>
  5c3c75: 4c 8d 75 d0                  	leaq	-0x30(%rbp), %r14
  5c3c79: 89 c2                        	movl	%eax, %edx
  5c3c7b: 48 8d 35 36 8d 45 04         	leaq	0x4458d36(%rip), %rsi   # 0x4a1c9b8 <plt_log10+0x4c468>
  5c3c82: 31 c0                        	xorl	%eax, %eax
  5c3c84: 4c 89 f7                     	movq	%r14, %rdi
  5c3c87: e8 f4 f3 a4 00               	callq	0x1013080 <module_init+0x1013060>
  5c3c8c: 83 7d d8 00                  	cmpl	$0x0, -0x28(%rbp)
  5c3c90: 74 06                        	je	0x5c3c98 <module_init+0x5c3c78>
  5c3c92: 4c 8b 45 d0                  	movq	-0x30(%rbp), %r8
  5c3c96: eb 07                        	jmp	0x5c3c9f <module_init+0x5c3c7f>
  5c3c98: 4c 8d 05 93 8c 45 04         	leaq	0x4458c93(%rip), %r8    # 0x4a1c932 <plt_log10+0x4c3e2>
  5c3c9f: 48 8d 3d 95 c7 45 04         	leaq	0x445c795(%rip), %rdi   # 0x4a2043b <plt_log10+0x4feeb>
  5c3ca6: 48 8d 15 f5 8c 45 04         	leaq	0x4458cf5(%rip), %rdx   # 0x4a1c9a2 <plt_log10+0x4c452>
  5c3cad: 48 8d 0d f8 8c 45 04         	leaq	0x4458cf8(%rip), %rcx   # 0x4a1c9ac <plt_log10+0x4c45c>
  5c3cb4: be 40 02 00 00               	movl	$0x240, %esi            # imm = 0x240
  5c3cb9: 31 c0                        	xorl	%eax, %eax
  5c3cbb: e8 e0 76 b7 00               	callq	0x113b3a0 <module_init+0x113b380>
  5c3cc0: 4c 89 f7                     	movq	%r14, %rdi
  5c3cc3: e8 78 b2 a4 ff               	callq	0xef40 <module_init+0xef20>
  5c3cc8: e8 a3 76 b7 00               	callq	0x113b370 <module_init+0x113b350>
  5c3ccd: 48 8d 05 bc ba 46 06         	leaq	0x646babc(%rip), %rax   # 0x6a2f790
  5c3cd4: 4c 8d 05 ad 3b 47 06         	leaq	0x6473bad(%rip), %r8    # 0x6a37888
  5c3cdb: 48 8d 35 a0 8c 45 04         	leaq	0x4458ca0(%rip), %rsi   # 0x4a1c982 <plt_log10+0x4c432>
  5c3ce2: 48 8d 15 57 8d 45 04         	leaq	0x4458d57(%rip), %rdx   # 0x4a1ca40 <plt_log10+0x4c4f0>
  5c3ce9: 48 8d 4d cc                  	leaq	-0x34(%rbp), %rcx
  5c3ced: c7 45 cc 00 00 00 00         	movl	$0x0, -0x34(%rbp)
  5c3cf4: 48 8b 38                     	movq	(%rax), %rdi
  5c3cf7: e8 94 d9 b8 00               	callq	0x1151690 <module_init+0x1151670>
  5c3cfc: 48 8d 7d c8                  	leaq	-0x38(%rbp), %rdi
  5c3d00: e8 db 8f 40 04               	callq	0x49ccce0 <plt_sceUserServiceGetInitialUser>
  5c3d05: 85 c0                        	testl	%eax, %eax
  5c3d07: 74 58                        	je	0x5c3d61 <module_init+0x5c3d41>
  5c3d09: 4c 8d 75 d0                  	leaq	-0x30(%rbp), %r14
  5c3d0d: 89 c2                        	movl	%eax, %edx
  5c3d0f: 48 8d 35 48 8d 45 04         	leaq	0x4458d48(%rip), %rsi   # 0x4a1ca5e <plt_log10+0x4c50e>
  5c3d16: 31 c0                        	xorl	%eax, %eax
  5c3d18: 4c 89 f7                     	movq	%r14, %rdi
  5c3d1b: e8 60 f3 a4 00               	callq	0x1013080 <module_init+0x1013060>
  5c3d20: 83 7d d8 00                  	cmpl	$0x0, -0x28(%rbp)
  5c3d24: 74 06                        	je	0x5c3d2c <module_init+0x5c3d0c>
  5c3d26: 4c 8b 45 d0                  	movq	-0x30(%rbp), %r8
  5c3d2a: eb 07                        	jmp	0x5c3d33 <module_init+0x5c3d13>
  5c3d2c: 4c 8d 05 ff 8b 45 04         	leaq	0x4458bff(%rip), %r8    # 0x4a1c932 <plt_log10+0x4c3e2>
  5c3d33: 48 8d 3d 01 c7 45 04         	leaq	0x445c701(%rip), %rdi   # 0x4a2043b <plt_log10+0x4feeb>
  5c3d3a: 48 8d 15 61 8c 45 04         	leaq	0x4458c61(%rip), %rdx   # 0x4a1c9a2 <plt_log10+0x4c452>
