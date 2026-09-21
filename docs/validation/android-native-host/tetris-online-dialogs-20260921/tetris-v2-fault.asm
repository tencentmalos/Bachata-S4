
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  e18f50: 79 11                        	jns	0xe18f63 <module_init+0xe18f43>
  e18f52: 44 24 40                     	andb	$0x40, %al
  e18f55: 8b 4d ac                     	movl	-0x54(%rbp), %ecx
  e18f58: 48 8b 83 f8 03 00 00         	movq	0x3f8(%rbx), %rax
  e18f5f: 83 f9 64                     	cmpl	$0x64, %ecx
  e18f62: 49 89 44 24 20               	movq	%rax, 0x20(%r12)
  e18f67: 45 89 6c 24 50               	movl	%r13d, 0x50(%r12)
  e18f6c: 49 89 34 24                  	movq	%rsi, (%r12)
  e18f70: 41 c7 84 24 c0 9f 00 00 00 00 00 00  	movl	$0x0, 0x9fc0(%r12)
  e18f7c: 45 89 bc 24 c4 9f 00 00      	movl	%r15d, 0x9fc4(%r12)
  e18f84: 0f 4c d1                     	cmovll	%ecx, %edx
  e18f87: b9 01 00 00 00               	movl	$0x1, %ecx
  e18f8c: 85 d2                        	testl	%edx, %edx
  e18f8e: 0f 4f ca                     	cmovgl	%edx, %ecx
  e18f91: 49 89 8c 24 c8 9f 00 00      	movq	%rcx, 0x9fc8(%r12)
  e18f99: 48 8b 45 a0                  	movq	-0x60(%rbp), %rax
  e18f9d: 48 8b 78 38                  	movq	0x38(%rax), %rdi
  e18fa1: 48 8b 07                     	movq	(%rdi), %rax
  e18fa4: ff 90 48 02 00 00            	callq	*0x248(%rax)
  e18faa: 48 89 c7                     	movq	%rax, %rdi
  e18fad: 4c 89 e6                     	movq	%r12, %rsi
  e18fb0: eb 3c                        	jmp	0xe18fee <module_init+0xe18fce>
  e18fb2: 31 c0                        	xorl	%eax, %eax
  e18fb4: 8b 4d ac                     	movl	-0x54(%rbp), %ecx
  e18fb7: 49 89 87 c8 9f 00 00         	movq	%rax, 0x9fc8(%r15)
  e18fbe: b8 64 00 00 00               	movl	$0x64, %eax
  e18fc3: 83 f9 64                     	cmpl	$0x64, %ecx
  e18fc6: 0f 4c c1                     	cmovll	%ecx, %eax
  e18fc9: b9 01 00 00 00               	movl	$0x1, %ecx
  e18fce: 85 c0                        	testl	%eax, %eax
  e18fd0: 0f 4f c8                     	cmovgl	%eax, %ecx
  e18fd3: 49 89 8f 70 a0 00 00         	movq	%rcx, 0xa070(%r15)
  e18fda: 49 8b 7c 24 38               	movq	0x38(%r12), %rdi
  e18fdf: 48 8b 07                     	movq	(%rdi), %rax
  e18fe2: ff 90 48 02 00 00            	callq	*0x248(%rax)
  e18fe8: 48 89 c7                     	movq	%rax, %rdi
  e18feb: 4c 89 fe                     	movq	%r15, %rsi
  e18fee: e8 7d 6b 6d ff               	callq	0x4efb70 <module_init+0x4efb50>
  e18ff3: 48 8b 5d b8                  	movq	-0x48(%rbp), %rbx
  e18ff7: 48 85 db                     	testq	%rbx, %rbx
  e18ffa: 74 1d                        	je	0xe19019 <module_init+0xe18ff9>
  e18ffc: f0                           	lock
  e18ffd: ff 4b 08                     	decl	0x8(%rbx)
  e19000: 75 17                        	jne	0xe19019 <module_init+0xe18ff9>
  e19002: 48 8b 03                     	movq	(%rbx), %rax
  e19005: 48 89 df                     	movq	%rbx, %rdi
  e19008: ff 10                        	callq	*(%rax)
  e1900a: f0                           	lock
  e1900b: ff 4b 0c                     	decl	0xc(%rbx)
  e1900e: 75 09                        	jne	0xe19019 <module_init+0xe18ff9>
  e19010: 48 8b 03                     	movq	(%rbx), %rax
  e19013: 48 89 df                     	movq	%rbx, %rdi
  e19016: ff 50 10                     	callq	*0x10(%rax)
  e19019: 48 8b 05 28 a0 75 05         	movq	0x575a028(%rip), %rax   # 0x6573048 <plt_log10+0x1ba2af8>
  e19020: 48 8b 00                     	movq	(%rax), %rax
  e19023: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
  e19027: 75 0f                        	jne	0xe19038 <module_init+0xe19018>
  e19029: 48 83 c4 38                  	addq	$0x38, %rsp
  e1902d: 5b                           	popq	%rbx
  e1902e: 41 5c                        	popq	%r12
  e19030: 41 5d                        	popq	%r13
  e19032: 41 5e                        	popq	%r14
  e19034: 41 5f                        	popq	%r15
  e19036: 5d                           	popq	%rbp
  e19037: c3                           	retq
  e19038: e8 73 23 bb 03               	callq	0x49cb3b0 <plt___stack_chk_fail>
  e1903d: 0f 0b                        	ud2
  e1903f: 90                           	nop
  e19040: 55                           	pushq	%rbp
  e19041: 48 89 e5                     	movq	%rsp, %rbp
  e19044: 41 57                        	pushq	%r15
  e19046: 41 56                        	pushq	%r14
  e19048: 41 55                        	pushq	%r13
  e1904a: 41 54                        	pushq	%r12
  e1904c: 53                           	pushq	%rbx
  e1904d: 48 83 ec 48                  	subq	$0x48, %rsp
  e19051: 48 8b 05 f0 9f 75 05         	movq	0x5759ff0(%rip), %rax   # 0x6573048 <plt_log10+0x1ba2af8>
  e19058: 48 89 7d a0                  	movq	%rdi, -0x60(%rbp)
  e1905c: 48 8d 35 5f a9 c9 03         	leaq	0x3c9a95f(%rip), %rsi   # 0x4ab39c2 <plt_log10+0xe3472>
  e19063: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  e19067: ba 01 00 00 00               	movl	$0x1, %edx
  e1906c: 48 8b 00                     	movq	(%rax), %rax
  e1906f: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
  e19073: e8 c8 1f 3e 00               	callq	0x11fb040 <module_init+0x11fb020>
  e19078: 8a 05 ca 62 81 05            	movb	0x58162ca(%rip), %al    # 0x662f348
  e1907e: 84 c0                        	testb	%al, %al
  e19080: 0f 84 55 02 00 00            	je	0xe192db <module_init+0xe192bb>
  e19086: 48 8b 1d b3 62 81 05         	movq	0x58162b3(%rip), %rbx   # 0x662f340
  e1908d: e8 6e 7c 39 00               	callq	0x11b0d00 <module_init+0x11b0ce0>
  e19092: 48 89 c7                     	movq	%rax, %rdi
  e19095: 48 89 de                     	movq	%rbx, %rsi
  e19098: e8 d3 8b 39 00               	callq	0x11b1c70 <module_init+0x11b1c50>
  e1909d: 48 8b 75 c0                  	movq	-0x40(%rbp), %rsi
  e190a1: 48 8b 08                     	movq	(%rax), %rcx
  e190a4: 48 89 c7                     	movq	%rax, %rdi
  e190a7: ff 51 58                     	callq	*0x58(%rcx)
  e190aa: 49 89 c7                     	movq	%rax, %r15
  e190ad: 48 8b 00                     	movq	(%rax), %rax
  e190b0: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
  e190b4: 4c 89 fe                     	movq	%r15, %rsi
  e190b7: ff 90 88 00 00 00            	callq	*0x88(%rax)
  e190bd: 48 8b 5d c8                  	movq	-0x38(%rbp), %rbx
  e190c1: 4c 8b 65 c0                  	movq	-0x40(%rbp), %r12
  e190c5: 48 85 db                     	testq	%rbx, %rbx
  e190c8: 74 1d                        	je	0xe190e7 <module_init+0xe190c7>
  e190ca: f0                           	lock
  e190cb: ff 4b 08                     	decl	0x8(%rbx)
  e190ce: 75 17                        	jne	0xe190e7 <module_init+0xe190c7>
  e190d0: 48 8b 03                     	movq	(%rbx), %rax
  e190d3: 48 89 df                     	movq	%rbx, %rdi
  e190d6: ff 10                        	callq	*(%rax)
  e190d8: f0                           	lock
  e190d9: ff 4b 0c                     	decl	0xc(%rbx)
  e190dc: 75 09                        	jne	0xe190e7 <module_init+0xe190c7>
  e190de: 48 8b 03                     	movq	(%rbx), %rax
  e190e1: 48 89 df                     	movq	%rbx, %rdi
  e190e4: ff 50 10                     	callq	*0x10(%rax)
  e190e7: 4d 85 e4                     	testq	%r12, %r12
  e190ea: 0f 84 14 01 00 00            	je	0xe19204 <module_init+0xe191e4>
  e190f0: 49 8b 07                     	movq	(%r15), %rax
  e190f3: 48 8d 7d b0                  	leaq	-0x50(%rbp), %rdi
  e190f7: 4c 89 fe                     	movq	%r15, %rsi
  e190fa: ff 90 88 00 00 00            	callq	*0x88(%rax)
  e19100: 48 8b 75 b0                  	movq	-0x50(%rbp), %rsi
  e19104: 4c 8d 6d c0                  	leaq	-0x40(%rbp), %r13
  e19108: 31 d2                        	xorl	%edx, %edx
  e1910a: 4c 89 ef                     	movq	%r13, %rdi
  e1910d: 48 8b 06                     	movq	(%rsi), %rax
  e19110: ff 90 f8 00 00 00            	callq	*0xf8(%rax)
  e19116: 48 8b 45 c8                  	movq	-0x38(%rbp), %rax
  e1911a: 4c 8b 75 c0                  	movq	-0x40(%rbp), %r14
  e1911e: 48 c7 45 c0 00 00 00 00      	movq	$0x0, -0x40(%rbp)
  e19126: 48 85 c0                     	testq	%rax, %rax
  e19129: 48 89 45 98                  	movq	%rax, -0x68(%rbp)
  e1912d: 74 08                        	je	0xe19137 <module_init+0xe19117>
  e1912f: 48 c7 45 c8 00 00 00 00      	movq	$0x0, -0x38(%rbp)
  e19137: 48 8b 5d b8                  	movq	-0x48(%rbp), %rbx
  e1913b: 48 85 db                     	testq	%rbx, %rbx
  e1913e: 74 1d                        	je	0xe1915d <module_init+0xe1913d>
  e19140: f0                           	lock
  e19141: ff 4b 08                     	decl	0x8(%rbx)
  e19144: 75 17                        	jne	0xe1915d <module_init+0xe1913d>
  e19146: 48 8b 03                     	movq	(%rbx), %rax
  e19149: 48 89 df                     	movq	%rbx, %rdi
  e1914c: ff 10                        	callq	*(%rax)
  e1914e: f0                           	lock
  e1914f: ff 4b 0c                     	decl	0xc(%rbx)
  e19152: 75 09                        	jne	0xe1915d <module_init+0xe1913d>
  e19154: 48 8b 03                     	movq	(%rbx), %rax
  e19157: 48 89 df                     	movq	%rbx, %rdi
  e1915a: ff 50 10                     	callq	*0x10(%rax)
  e1915d: 49 8b 07                     	movq	(%r15), %rax
  e19160: 4c 89 ef                     	movq	%r13, %rdi
  e19163: 4c 89 fe                     	movq	%r15, %rsi
  e19166: ff 90 b8 00 00 00            	callq	*0xb8(%rax)
  e1916c: 4c 8b 65 a0                  	movq	-0x60(%rbp), %r12
  e19170: 49 8d 84 24 d0 05 00 00      	leaq	0x5d0(%r12), %rax
  e19178: 4c 39 e8                     	cmpq	%r13, %rax
  e1917b: 74 57                        	je	0xe191d4 <module_init+0xe191b4>
  e1917d: 48 8b 45 c0                  	movq	-0x40(%rbp), %rax
