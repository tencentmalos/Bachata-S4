
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 3c50200: b1 d7                        	movb	$-0x29, %cl
 3c50202: 00 0f                        	addb	%cl, (%rdi)
 3c50204: 0b 90 90 90 90 90            	orl	-0x6f6f6f70(%rax), %edx
 3c5020a: 90                           	nop
 3c5020b: 90                           	nop
 3c5020c: 90                           	nop
 3c5020d: 90                           	nop
 3c5020e: 90                           	nop
 3c5020f: 90                           	nop
 3c50210: 55                           	pushq	%rbp
 3c50211: 48 89 e5                     	movq	%rsp, %rbp
 3c50214: 41 57                        	pushq	%r15
 3c50216: 41 56                        	pushq	%r14
 3c50218: 53                           	pushq	%rbx
 3c50219: 48 83 ec 78                  	subq	$0x78, %rsp
 3c5021d: 4c 8b 35 24 2e 92 02         	movq	0x2922e24(%rip), %r14   # 0x6573048 <plt_log10+0x1ba2af8>
 3c50224: 48 89 f8                     	movq	%rdi, %rax
 3c50227: 48 83 c7 08                  	addq	$0x8, %rdi
 3c5022b: 49 8b 0e                     	movq	(%r14), %rcx
 3c5022e: 48 89 4d e0                  	movq	%rcx, -0x20(%rbp)
 3c50232: 89 b5 74 ff ff ff            	movl	%esi, -0x8c(%rbp)
 3c50238: 8b 48 10                     	movl	0x10(%rax), %ecx
 3c5023b: 3b 48 3c                     	cmpl	0x3c(%rax), %ecx
 3c5023e: 74 48                        	je	0x3c50288 <module_init+0x3c50268>
 3c50240: 48 8b 50 48                  	movq	0x48(%rax), %rdx
 3c50244: 48 8d 48 40                  	leaq	0x40(%rax), %rcx
 3c50248: 48 63 40 50                  	movslq	0x50(%rax), %rax
 3c5024c: 48 85 d2                     	testq	%rdx, %rdx
 3c5024f: 48 0f 45 ca                  	cmovneq	%rdx, %rcx
 3c50253: 48 ff c8                     	decq	%rax
 3c50256: 48 63 d6                     	movslq	%esi, %rdx
 3c50259: 48 21 c2                     	andq	%rax, %rdx
 3c5025c: 8b 04 91                     	movl	(%rcx,%rdx,4), %eax
 3c5025f: 83 f8 ff                     	cmpl	$-0x1, %eax
 3c50262: 74 24                        	je	0x3c50288 <module_init+0x3c50268>
 3c50264: 48 8b 1f                     	movq	(%rdi), %rbx
 3c50267: 66 0f 1f 84 00 00 00 00 00   	nopw	(%rax,%rax)
 3c50270: 48 98                        	cltq
 3c50272: 48 6b c0 68                  	imulq	$0x68, %rax, %rax
 3c50276: 39 34 03                     	cmpl	%esi, (%rbx,%rax)
 3c50279: 0f 84 82 00 00 00            	je	0x3c50301 <module_init+0x3c502e1>
 3c5027f: 8b 44 03 60                  	movl	0x60(%rbx,%rax), %eax
 3c50283: 83 f8 ff                     	cmpl	$-0x1, %eax
 3c50286: 75 e8                        	jne	0x3c50270 <module_init+0x3c50250>
 3c50288: c5 f8 10 0d d0 ff 1b 01      	vmovups	0x11bffd0(%rip), %xmm1  # 0x4e10260 <plt_log10+0x43fd10>
 3c50290: 48 8d 85 74 ff ff ff         	leaq	-0x8c(%rbp), %rax
 3c50297: 48 8d b5 78 ff ff ff         	leaq	-0x88(%rbp), %rsi
 3c5029e: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c502a2: c5 f8 11 45 90               	vmovups	%xmm0, -0x70(%rbp)
 3c502a7: 48 c7 45 b0 00 00 00 00      	movq	$0x0, -0x50(%rbp)
 3c502af: 31 d2                        	xorl	%edx, %edx
 3c502b1: 48 8d 5d 90                  	leaq	-0x70(%rbp), %rbx
 3c502b5: 48 89 85 78 ff ff ff         	movq	%rax, -0x88(%rbp)
 3c502bc: 48 8d 45 88                  	leaq	-0x78(%rbp), %rax
 3c502c0: 48 89 45 80                  	movq	%rax, -0x80(%rbp)
 3c502c4: c5 f8 11 4d b8               	vmovups	%xmm1, -0x48(%rbp)
 3c502c9: 48 c7 45 d0 00 00 00 00      	movq	$0x0, -0x30(%rbp)
 3c502d1: c7 45 d8 00 00 00 00         	movl	$0x0, -0x28(%rbp)
 3c502d8: c6 45 88 01                  	movb	$0x1, -0x78(%rbp)
 3c502dc: e8 0f 45 00 00               	callq	0x3c547f0 <module_init+0x3c547d0>
 3c502e1: 48 89 df                     	movq	%rbx, %rdi
 3c502e4: e8 f7 3d 00 00               	callq	0x3c540e0 <module_init+0x3c540c0>
 3c502e9: 49 8b 06                     	movq	(%r14), %rax
 3c502ec: 48 3b 45 e0                  	cmpq	-0x20(%rbp), %rax
 3c502f0: 0f 85 63 01 00 00            	jne	0x3c50459 <module_init+0x3c50439>
 3c502f6: 48 83 c4 78                  	addq	$0x78, %rsp
 3c502fa: 5b                           	popq	%rbx
 3c502fb: 41 5e                        	popq	%r14
 3c502fd: 41 5f                        	popq	%r15
 3c502ff: 5d                           	popq	%rbp
 3c50300: c3                           	retq
 3c50301: c6 44 03 08 01               	movb	$0x1, 0x8(%rbx,%rax)
 3c50306: 4c 8d 54 03 20               	leaq	0x20(%rbx,%rax), %r10
 3c5030b: 44 8b 5c 03 38               	movl	0x38(%rbx,%rax), %r11d
 3c50310: 45 85 db                     	testl	%r11d, %r11d
 3c50313: 74 77                        	je	0x3c5038c <module_init+0x3c5036c>
 3c50315: 48 8b 4c 03 30               	movq	0x30(%rbx,%rax), %rcx
 3c5031a: 41 8d 53 ff                  	leal	-0x1(%r11), %edx
 3c5031e: 45 8d 43 1e                  	leal	0x1e(%r11), %r8d
 3c50322: be 00 00 00 00               	movl	$0x0, %esi
 3c50327: 48 85 c9                     	testq	%rcx, %rcx
 3c5032a: 49 0f 44 ca                  	cmoveq	%r10, %rcx
 3c5032e: 85 d2                        	testl	%edx, %edx
 3c50330: 8b 39                        	movl	(%rcx), %edi
 3c50332: 44 0f 49 c2                  	cmovnsl	%edx, %r8d
 3c50336: 31 d2                        	xorl	%edx, %edx
 3c50338: 85 ff                        	testl	%edi, %edi
 3c5033a: 75 27                        	jne	0x3c50363 <module_init+0x3c50343>
 3c5033c: 41 c1 f8 05                  	sarl	$0x5, %r8d
 3c50340: 31 f6                        	xorl	%esi, %esi
 3c50342: 44 89 c2                     	movl	%r8d, %edx
 3c50345: c1 fa 1f                     	sarl	$0x1f, %edx
 3c50348: c4 42 68 f2 c0               	andnl	%r8d, %edx, %r8d
 3c5034d: 31 d2                        	xorl	%edx, %edx
 3c5034f: 90                           	nop
 3c50350: 49 39 f0                     	cmpq	%rsi, %r8
 3c50353: 74 94                        	je	0x3c502e9 <module_init+0x3c502c9>
 3c50355: 8b 7c b1 04                  	movl	0x4(%rcx,%rsi,4), %edi
 3c50359: 83 c2 20                     	addl	$0x20, %edx
 3c5035c: 48 ff c6                     	incq	%rsi
 3c5035f: 85 ff                        	testl	%edi, %edi
 3c50361: 74 ed                        	je	0x3c50350 <module_init+0x3c50330>
 3c50363: c4 e2 40 f3 df               	blsil	%edi, %edi
 3c50368: 48 8d 4c 3f 01               	leaq	0x1(%rdi,%rdi), %rcx
 3c5036d: f3 4c 0f bd c1               	lzcntq	%rcx, %r8
 3c50372: 89 d1                        	movl	%edx, %ecx
 3c50374: 44 29 c1                     	subl	%r8d, %ecx
 3c50377: 83 c1 3e                     	addl	$0x3e, %ecx
 3c5037a: 44 39 d9                     	cmpl	%r11d, %ecx
 3c5037d: 41 0f 4f cb                  	cmovgl	%r11d, %ecx
 3c50381: 44 39 d9                     	cmpl	%r11d, %ecx
 3c50384: 0f 84 5f ff ff ff            	je	0x3c502e9 <module_init+0x3c502c9>
 3c5038a: eb 14                        	jmp	0x3c503a0 <module_init+0x3c50380>
 3c5038c: bf 01 00 00 00               	movl	$0x1, %edi
 3c50391: 31 c9                        	xorl	%ecx, %ecx
 3c50393: 31 d2                        	xorl	%edx, %edx
 3c50395: 31 f6                        	xorl	%esi, %esi
 3c50397: 44 39 d9                     	cmpl	%r11d, %ecx
 3c5039a: 0f 84 49 ff ff ff            	je	0x3c502e9 <module_init+0x3c502c9>
 3c503a0: 4c 8b 4c 03 10               	movq	0x10(%rbx,%rax), %r9
 3c503a5: 48 8b 44 03 30               	movq	0x30(%rbx,%rax), %rax
 3c503aa: 45 8d 7b 1e                  	leal	0x1e(%r11), %r15d
 3c503ae: 48 85 c0                     	testq	%rax, %rax
 3c503b1: 4c 0f 45 d0                  	cmovneq	%rax, %r10
 3c503b5: 41 8d 43 ff                  	leal	-0x1(%r11), %eax
 3c503b9: 85 c0                        	testl	%eax, %eax
 3c503bb: 4d 8d 42 04                  	leaq	0x4(%r10), %r8
 3c503bf: 44 0f 49 f8                  	cmovnsl	%eax, %r15d
 3c503c3: b8 ff ff ff ff               	movl	$0xffffffff, %eax       # imm = 0xFFFFFFFF
 3c503c8: 41 c1 ff 05                  	sarl	$0x5, %r15d
 3c503cc: eb 2d                        	jmp	0x3c503fb <module_init+0x3c503db>
 3c503ce: b8 ff ff ff ff               	movl	$0xffffffff, %eax       # imm = 0xFFFFFFFF
 3c503d3: c4 e2 40 f3 df               	blsil	%edi, %edi
 3c503d8: 89 d3                        	movl	%edx, %ebx
 3c503da: 48 8d 4c 3f 01               	leaq	0x1(%rdi,%rdi), %rcx
 3c503df: f3 48 0f bd c9               	lzcntq	%rcx, %rcx
 3c503e4: 29 cb                        	subl	%ecx, %ebx
 3c503e6: 83 c3 3e                     	addl	$0x3e, %ebx
 3c503e9: 44 39 db                     	cmpl	%r11d, %ebx
 3c503ec: 89 d9                        	movl	%ebx, %ecx
 3c503ee: 41 0f 4f cb                  	cmovgl	%r11d, %ecx
 3c503f2: 41 39 db                     	cmpl	%ebx, %r11d
 3c503f5: 0f 8e ee fe ff ff            	jle	0x3c502e9 <module_init+0x3c502c9>
 3c503fb: 48 63 c9                     	movslq	%ecx, %rcx
 3c503fe: 48 63 de                     	movslq	%esi, %rbx
 3c50401: c4 e2 40 f2 c0               	andnl	%eax, %edi, %eax
 3c50406: 48 c1 e1 04                  	shlq	$0x4, %rcx
 3c5040a: 41 c6 44 09 04 00            	movb	$0x0, 0x4(%r9,%rcx)
 3c50410: 41 8b 3c 9a                  	movl	(%r10,%rbx,4), %edi
 3c50414: 21 c7                        	andl	%eax, %edi
 3c50416: 75 bb                        	jne	0x3c503d3 <module_init+0x3c503b3>
 3c50418: 44 39 fe                     	cmpl	%r15d, %esi
 3c5041b: 44 89 f8                     	movl	%r15d, %eax
 3c5041e: 0f 4f c6                     	cmovgl	%esi, %eax
 3c50421: 0f 8d c2 fe ff ff            	jge	0x3c502e9 <module_init+0x3c502c9>
 3c50427: 48 98                        	cltq
 3c50429: 83 c2 20                     	addl	$0x20, %edx
 3c5042c: ff c6                        	incl	%esi
 3c5042e: 49 8d 0c 98                  	leaq	(%r8,%rbx,4), %rcx
 3c50432: 48 29 d8                     	subq	%rbx, %rax
 3c50435: 66 66 2e 0f 1f 84 00 00 00 00 00     	nopw	%cs:(%rax,%rax)
 3c50440: 8b 39                        	movl	(%rcx), %edi
 3c50442: 85 ff                        	testl	%edi, %edi
 3c50444: 75 88                        	jne	0x3c503ce <module_init+0x3c503ae>
 3c50446: 83 c2 20                     	addl	$0x20, %edx
 3c50449: ff c6                        	incl	%esi
 3c5044b: 48 83 c1 04                  	addq	$0x4, %rcx
 3c5044f: 48 ff c8                     	decq	%rax
 3c50452: 75 ec                        	jne	0x3c50440 <module_init+0x3c50420>
 3c50454: e9 90 fe ff ff               	jmp	0x3c502e9 <module_init+0x3c502c9>
 3c50459: e8 52 af d7 00               	callq	0x49cb3b0 <plt___stack_chk_fail>
 3c5045e: 0f 0b                        	ud2
 3c50460: 55                           	pushq	%rbp
 3c50461: 48 89 e5                     	movq	%rsp, %rbp
 3c50464: 41 57                        	pushq	%r15
 3c50466: 41 56                        	pushq	%r14
 3c50468: 41 55                        	pushq	%r13
 3c5046a: 41 54                        	pushq	%r12
 3c5046c: 53                           	pushq	%rbx
 3c5046d: c6 07 00                     	movb	$0x0, (%rdi)
 3c50470: 4c 8d 47 18                  	leaq	0x18(%rdi), %r8
 3c50474: 44 8b 4f 30                  	movl	0x30(%rdi), %r9d
 3c50478: 45 85 c9                     	testl	%r9d, %r9d
 3c5047b: 74 7d                        	je	0x3c504fa <module_init+0x3c504da>
 3c5047d: 48 8b 47 28                  	movq	0x28(%rdi), %rax
 3c50481: 41 8d 49 ff                  	leal	-0x1(%r9), %ecx
 3c50485: 41 8d 51 1e                  	leal	0x1e(%r9), %edx
 3c50489: 41 bc 00 00 00 00            	movl	$0x0, %r12d
 3c5048f: 48 85 c0                     	testq	%rax, %rax
 3c50492: 49 0f 44 c0                  	cmoveq	%r8, %rax
 3c50496: 85 c9                        	testl	%ecx, %ecx
 3c50498: 0f 49 d1                     	cmovnsl	%ecx, %edx
 3c5049b: 8b 08                        	movl	(%rax), %ecx
 3c5049d: 45 31 ff                     	xorl	%r15d, %r15d
 3c504a0: 85 c9                        	testl	%ecx, %ecx
 3c504a2: 75 31                        	jne	0x3c504d5 <module_init+0x3c504b5>
 3c504a4: c1 fa 05                     	sarl	$0x5, %edx
 3c504a7: 45 31 e4                     	xorl	%r12d, %r12d
 3c504aa: 45 31 ff                     	xorl	%r15d, %r15d
 3c504ad: 89 d1                        	movl	%edx, %ecx
 3c504af: c1 f9 1f                     	sarl	$0x1f, %ecx
 3c504b2: c4 e2 70 f2 d2               	andnl	%edx, %ecx, %edx
 3c504b7: 66 0f 1f 84 00 00 00 00 00   	nopw	(%rax,%rax)
 3c504c0: 4c 39 e2                     	cmpq	%r12, %rdx
 3c504c3: 74 47                        	je	0x3c5050c <module_init+0x3c504ec>
 3c504c5: 42 8b 4c a0 04               	movl	0x4(%rax,%r12,4), %ecx
 3c504ca: 41 83 c7 20                  	addl	$0x20, %r15d
 3c504ce: 49 ff c4                     	incq	%r12
 3c504d1: 85 c9                        	testl	%ecx, %ecx
 3c504d3: 74 eb                        	je	0x3c504c0 <module_init+0x3c504a0>
 3c504d5: c4 e2 60 f3 d9               	blsil	%ecx, %ebx
 3c504da: 48 8d 44 1b 01               	leaq	0x1(%rbx,%rbx), %rax
 3c504df: f3 48 0f bd c8               	lzcntq	%rax, %rcx
 3c504e4: 44 89 f8                     	movl	%r15d, %eax
 3c504e7: 29 c8                        	subl	%ecx, %eax
 3c504e9: 83 c0 3e                     	addl	$0x3e, %eax
 3c504ec: 44 39 c8                     	cmpl	%r9d, %eax
 3c504ef: 41 0f 4f c1                  	cmovgl	%r9d, %eax
 3c504f3: 44 39 c8                     	cmpl	%r9d, %eax
 3c504f6: 74 14                        	je	0x3c5050c <module_init+0x3c504ec>
 3c504f8: eb 1d                        	jmp	0x3c50517 <module_init+0x3c504f7>
 3c504fa: bb 01 00 00 00               	movl	$0x1, %ebx
 3c504ff: 31 c0                        	xorl	%eax, %eax
 3c50501: 45 31 ff                     	xorl	%r15d, %r15d
 3c50504: 45 31 e4                     	xorl	%r12d, %r12d
 3c50507: 44 39 c8                     	cmpl	%r9d, %eax
 3c5050a: 75 0b                        	jne	0x3c50517 <module_init+0x3c504f7>
 3c5050c: 5b                           	popq	%rbx
 3c5050d: 41 5c                        	popq	%r12
 3c5050f: 41 5d                        	popq	%r13
 3c50511: 41 5e                        	popq	%r14
 3c50513: 41 5f                        	popq	%r15
 3c50515: 5d                           	popq	%rbp
 3c50516: c3                           	retq
 3c50517: 48 8b 4f 28                  	movq	0x28(%rdi), %rcx
 3c5051b: 41 8d 71 ff                  	leal	-0x1(%r9), %esi
 3c5051f: 4c 8b 5f 08                  	movq	0x8(%rdi), %r11
 3c50523: 41 8d 51 1e                  	leal	0x1e(%r9), %edx
 3c50527: 4c 89 4d c0                  	movq	%r9, -0x40(%rbp)
 3c5052b: 48 85 c9                     	testq	%rcx, %rcx
 3c5052e: 4c 89 5d d0                  	movq	%r11, -0x30(%rbp)
 3c50532: 4c 0f 45 c1                  	cmovneq	%rcx, %r8
 3c50536: 85 f6                        	testl	%esi, %esi
 3c50538: 0f 49 d6                     	cmovnsl	%esi, %edx
 3c5053b: 49 8d 48 04                  	leaq	0x4(%r8), %rcx
 3c5053f: be ff ff ff ff               	movl	$0xffffffff, %esi       # imm = 0xFFFFFFFF
 3c50544: 4c 89 45 a8                  	movq	%r8, -0x58(%rbp)
 3c50548: c1 fa 05                     	sarl	$0x5, %edx
 3c5054b: 48 89 4d b8                  	movq	%rcx, -0x48(%rbp)
 3c5054f: 89 55 cc                     	movl	%edx, -0x34(%rbp)
 3c50552: 48 98                        	cltq
 3c50554: 89 75 c8                     	movl	%esi, -0x38(%rbp)
 3c50557: 48 89 5d a0                  	movq	%rbx, -0x60(%rbp)
 3c5055b: 4c 6b c8 68                  	imulq	$0x68, %rax, %r9
 3c5055f: 43 c6 44 0b 08 01            	movb	$0x1, 0x8(%r11,%r9)
 3c50565: 4f 8d 44 0b 20               	leaq	0x20(%r11,%r9), %r8
 3c5056a: 47 8b 54 0b 38               	movl	0x38(%r11,%r9), %r10d
 3c5056f: 45 85 d2                     	testl	%r10d, %r10d
 3c50572: 0f 84 88 00 00 00            	je	0x3c50600 <module_init+0x3c505e0>
 3c50578: 4b 8b 5c 0b 30               	movq	0x30(%r11,%r9), %rbx
 3c5057d: 41 8d 42 ff                  	leal	-0x1(%r10), %eax
 3c50581: 41 8d 52 1e                  	leal	0x1e(%r10), %edx
 3c50585: b9 00 00 00 00               	movl	$0x0, %ecx
 3c5058a: 48 85 db                     	testq	%rbx, %rbx
 3c5058d: 49 0f 44 d8                  	cmoveq	%r8, %rbx
 3c50591: 85 c0                        	testl	%eax, %eax
 3c50593: 8b 3b                        	movl	(%rbx), %edi
 3c50595: 0f 49 d0                     	cmovnsl	%eax, %edx
 3c50598: 31 c0                        	xorl	%eax, %eax
 3c5059a: 85 ff                        	testl	%edi, %edi
 3c5059c: 75 29                        	jne	0x3c505c7 <module_init+0x3c505a7>
 3c5059e: c1 fa 05                     	sarl	$0x5, %edx
 3c505a1: 31 c9                        	xorl	%ecx, %ecx
 3c505a3: 89 d0                        	movl	%edx, %eax
 3c505a5: c1 f8 1f                     	sarl	$0x1f, %eax
 3c505a8: c4 e2 78 f2 d2               	andnl	%edx, %eax, %edx
 3c505ad: 31 c0                        	xorl	%eax, %eax
 3c505af: 90                           	nop
 3c505b0: 48 39 ca                     	cmpq	%rcx, %rdx
 3c505b3: 0f 84 1b 01 00 00            	je	0x3c506d4 <module_init+0x3c506b4>
 3c505b9: 8b 7c 8b 04                  	movl	0x4(%rbx,%rcx,4), %edi
 3c505bd: 83 c0 20                     	addl	$0x20, %eax
 3c505c0: 48 ff c1                     	incq	%rcx
 3c505c3: 85 ff                        	testl	%edi, %edi
 3c505c5: 74 e9                        	je	0x3c505b0 <module_init+0x3c50590>
 3c505c7: c4 e2 20 f3 df               	blsil	%edi, %r11d
 3c505cc: 41 89 c6                     	movl	%eax, %r14d
 3c505cf: 4b 8d 54 1b 01               	leaq	0x1(%r11,%r11), %rdx
 3c505d4: f3 48 0f bd d2               	lzcntq	%rdx, %rdx
 3c505d9: 41 29 d6                     	subl	%edx, %r14d
 3c505dc: 41 83 c6 3e                  	addl	$0x3e, %r14d
 3c505e0: 45 39 d6                     	cmpl	%r10d, %r14d
 3c505e3: 45 0f 4f f2                  	cmovgl	%r10d, %r14d
 3c505e7: 45 39 d6                     	cmpl	%r10d, %r14d
 3c505ea: 75 2a                        	jne	0x3c50616 <module_init+0x3c505f6>
 3c505ec: e9 e3 00 00 00               	jmp	0x3c506d4 <module_init+0x3c506b4>
 3c505f1: 66 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00 	nopw	%cs:(%rax,%rax)
 3c50600: 41 bb 01 00 00 00            	movl	$0x1, %r11d
 3c50606: 45 31 f6                     	xorl	%r14d, %r14d
 3c50609: 31 c0                        	xorl	%eax, %eax
 3c5060b: 31 c9                        	xorl	%ecx, %ecx
 3c5060d: 45 39 d6                     	cmpl	%r10d, %r14d
 3c50610: 0f 84 be 00 00 00            	je	0x3c506d4 <module_init+0x3c506b4>
 3c50616: 48 8b 75 d0                  	movq	-0x30(%rbp), %rsi
 3c5061a: bb ff ff ff ff               	movl	$0xffffffff, %ebx       # imm = 0xFFFFFFFF
 3c5061f: 4a 8b 7c 0e 30               	movq	0x30(%rsi,%r9), %rdi
 3c50624: 4a 8d 54 0e 10               	leaq	0x10(%rsi,%r9), %rdx
 3c50629: 41 8d 72 ff                  	leal	-0x1(%r10), %esi
 3c5062d: 45 8d 4a 1e                  	leal	0x1e(%r10), %r9d
 3c50631: 4c 8b 2a                     	movq	(%rdx), %r13
 3c50634: 48 85 ff                     	testq	%rdi, %rdi
 3c50637: 4c 0f 45 c7                  	cmovneq	%rdi, %r8
 3c5063b: 85 f6                        	testl	%esi, %esi
 3c5063d: 44 0f 49 ce                  	cmovnsl	%esi, %r9d
 3c50641: 49 8d 50 04                  	leaq	0x4(%r8), %rdx
 3c50645: 41 c1 f9 05                  	sarl	$0x5, %r9d
 3c50649: 48 89 55 b0                  	movq	%rdx, -0x50(%rbp)
 3c5064d: eb 2a                        	jmp	0x3c50679 <module_init+0x3c50659>
 3c5064f: bb ff ff ff ff               	movl	$0xffffffff, %ebx       # imm = 0xFFFFFFFF
 3c50654: c4 e2 20 f3 da               	blsil	%edx, %r11d
 3c50659: 89 c6                        	movl	%eax, %esi
 3c5065b: 4b 8d 54 1b 01               	leaq	0x1(%r11,%r11), %rdx
 3c50660: f3 48 0f bd d2               	lzcntq	%rdx, %rdx
 3c50665: 29 d6                        	subl	%edx, %esi
 3c50667: 83 c6 3e                     	addl	$0x3e, %esi
 3c5066a: 44 39 d6                     	cmpl	%r10d, %esi
 3c5066d: 41 89 f6                     	movl	%esi, %r14d
 3c50670: 45 0f 4f f2                  	cmovgl	%r10d, %r14d
 3c50674: 41 39 f2                     	cmpl	%esi, %r10d
 3c50677: 7e 5b                        	jle	0x3c506d4 <module_init+0x3c506b4>
 3c50679: 49 63 d6                     	movslq	%r14d, %rdx
 3c5067c: 48 63 f1                     	movslq	%ecx, %rsi
 3c5067f: c4 e2 20 f2 db               	andnl	%ebx, %r11d, %ebx
 3c50684: 48 c1 e2 04                  	shlq	$0x4, %rdx
 3c50688: 41 c6 44 15 04 00            	movb	$0x0, 0x4(%r13,%rdx)
 3c5068e: 41 8b 14 b0                  	movl	(%r8,%rsi,4), %edx
 3c50692: 21 da                        	andl	%ebx, %edx
 3c50694: 75 be                        	jne	0x3c50654 <module_init+0x3c50634>
 3c50696: 44 39 c9                     	cmpl	%r9d, %ecx
 3c50699: 44 89 ca                     	movl	%r9d, %edx
 3c5069c: 0f 4f d1                     	cmovgl	%ecx, %edx
 3c5069f: 7d 33                        	jge	0x3c506d4 <module_init+0x3c506b4>
 3c506a1: 48 8b 7d b0                  	movq	-0x50(%rbp), %rdi
 3c506a5: 48 63 da                     	movslq	%edx, %rbx
 3c506a8: 83 c0 20                     	addl	$0x20, %eax
 3c506ab: ff c1                        	incl	%ecx
 3c506ad: 48 29 f3                     	subq	%rsi, %rbx
 3c506b0: 48 8d 3c b7                  	leaq	(%rdi,%rsi,4), %rdi
 3c506b4: 66 66 66 2e 0f 1f 84 00 00 00 00 00  	nopw	%cs:(%rax,%rax)
 3c506c0: 8b 17                        	movl	(%rdi), %edx
 3c506c2: 85 d2                        	testl	%edx, %edx
 3c506c4: 75 89                        	jne	0x3c5064f <module_init+0x3c5062f>
 3c506c6: 83 c0 20                     	addl	$0x20, %eax
 3c506c9: ff c1                        	incl	%ecx
 3c506cb: 48 83 c7 04                  	addq	$0x4, %rdi
 3c506cf: 48 ff cb                     	decq	%rbx
 3c506d2: 75 ec                        	jne	0x3c506c0 <module_init+0x3c506a0>
 3c506d4: 4c 8b 45 a8                  	movq	-0x58(%rbp), %r8
 3c506d8: 49 63 d4                     	movslq	%r12d, %rdx
 3c506db: 48 8b 45 a0                  	movq	-0x60(%rbp), %rax
 3c506df: 8b 75 c8                     	movl	-0x38(%rbp), %esi
 3c506e2: 41 8b 3c 90                  	movl	(%r8,%rdx,4), %edi
 3c506e6: c4 e2 78 f2 f6               	andnl	%esi, %eax, %esi
 3c506eb: 21 f7                        	andl	%esi, %edi
 3c506ed: 74 11                        	je	0x3c50700 <module_init+0x3c506e0>
 3c506ef: 4c 8b 4d c0                  	movq	-0x40(%rbp), %r9
 3c506f3: 4c 8b 5d d0                  	movq	-0x30(%rbp), %r11
 3c506f7: eb 57                        	jmp	0x3c50750 <module_init+0x3c50730>
 3c506f9: 0f 1f 80 00 00 00 00         	nopl	(%rax)
 3c50700: 8b 45 cc                     	movl	-0x34(%rbp), %eax
 3c50703: 4c 8b 4d c0                  	movq	-0x40(%rbp), %r9
 3c50707: 4c 8b 5d d0                  	movq	-0x30(%rbp), %r11
 3c5070b: 41 39 c4                     	cmpl	%eax, %r12d
 3c5070e: 41 0f 4f c4                  	cmovgl	%r12d, %eax
 3c50712: 0f 8d f4 fd ff ff            	jge	0x3c5050c <module_init+0x3c504ec>
 3c50718: 48 8b 4d b8                  	movq	-0x48(%rbp), %rcx
 3c5071c: 48 98                        	cltq
 3c5071e: 41 83 c7 20                  	addl	$0x20, %r15d
 3c50722: 41 ff c4                     	incl	%r12d
 3c50725: 48 29 d0                     	subq	%rdx, %rax
 3c50728: 48 8d 0c 91                  	leaq	(%rcx,%rdx,4), %rcx
 3c5072c: 0f 1f 40 00                  	nopl	(%rax)
 3c50730: 8b 39                        	movl	(%rcx), %edi
 3c50732: 85 ff                        	testl	%edi, %edi
 3c50734: 75 15                        	jne	0x3c5074b <module_init+0x3c5072b>
 3c50736: 41 83 c7 20                  	addl	$0x20, %r15d
 3c5073a: 41 ff c4                     	incl	%r12d
 3c5073d: 48 83 c1 04                  	addq	$0x4, %rcx
 3c50741: 48 ff c8                     	decq	%rax
 3c50744: 75 ea                        	jne	0x3c50730 <module_init+0x3c50710>
 3c50746: e9 c1 fd ff ff               	jmp	0x3c5050c <module_init+0x3c504ec>
 3c5074b: be ff ff ff ff               	movl	$0xffffffff, %esi       # imm = 0xFFFFFFFF
 3c50750: c4 e2 60 f3 df               	blsil	%edi, %ebx
 3c50755: 44 89 f9                     	movl	%r15d, %ecx
 3c50758: 48 8d 44 1b 01               	leaq	0x1(%rbx,%rbx), %rax
 3c5075d: f3 48 0f bd c0               	lzcntq	%rax, %rax
 3c50762: 29 c1                        	subl	%eax, %ecx
 3c50764: 83 c1 3e                     	addl	$0x3e, %ecx
 3c50767: 44 39 c9                     	cmpl	%r9d, %ecx
 3c5076a: 89 c8                        	movl	%ecx, %eax
 3c5076c: 41 0f 4f c1                  	cmovgl	%r9d, %eax
 3c50770: 41 39 c9                     	cmpl	%ecx, %r9d
 3c50773: 0f 8f d9 fd ff ff            	jg	0x3c50552 <module_init+0x3c50532>
 3c50779: e9 8e fd ff ff               	jmp	0x3c5050c <module_init+0x3c504ec>
 3c5077e: 90                           	nop
 3c5077f: 90                           	nop
 3c50780: 55                           	pushq	%rbp
 3c50781: 48 89 e5                     	movq	%rsp, %rbp
 3c50784: 41 57                        	pushq	%r15
 3c50786: 41 56                        	pushq	%r14
 3c50788: 41 55                        	pushq	%r13
 3c5078a: 41 54                        	pushq	%r12
 3c5078c: 53                           	pushq	%rbx
 3c5078d: 48 83 ec 18                  	subq	$0x18, %rsp
 3c50791: 4c 8b 2d b0 28 92 02         	movq	0x29228b0(%rip), %r13   # 0x6573048 <plt_log10+0x1ba2af8>
 3c50798: c5 f8 10 05 d0 fa 1b 01      	vmovups	0x11bfad0(%rip), %xmm0  # 0x4e10270 <plt_log10+0x43fd20>
 3c507a0: c5 f8 10 0d d8 fa 1b 01      	vmovups	0x11bfad8(%rip), %xmm1  # 0x4e10280 <plt_log10+0x43fd30>
 3c507a8: 48 8d 15 a1 f9 81 02         	leaq	0x281f9a1(%rip), %rdx   # 0x6470150 <plt_log10+0x1a9fc00>
 3c507af: 48 8d 0d 62 fa 81 02         	leaq	0x281fa62(%rip), %rcx   # 0x6470218 <plt_log10+0x1a9fcc8>
 3c507b6: 4c 8d 65 c8                  	leaq	-0x38(%rbp), %r12
 3c507ba: 48 89 fb                     	movq	%rdi, %rbx
 3c507bd: c5 e8 57 d2                  	vxorps	%xmm2, %xmm2, %xmm2
 3c507c1: 4c 8d b7 90 00 00 00         	leaq	0x90(%rdi), %r14
 3c507c8: 4c 8d bf 38 0e 00 00         	leaq	0xe38(%rdi), %r15
 3c507cf: 49 8b 45 00                  	movq	(%r13), %rax
 3c507d3: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
 3c507d7: 48 8d 05 8a fa 81 02         	leaq	0x281fa8a(%rip), %rax   # 0x6470268 <plt_log10+0x1a9fd18>
 3c507de: c5 f8 11 57 18               	vmovups	%xmm2, 0x18(%rdi)
 3c507e3: c5 f8 11 57 28               	vmovups	%xmm2, 0x28(%rdi)
 3c507e8: 48 89 17                     	movq	%rdx, (%rdi)
 3c507eb: 48 89 4f 08                  	movq	%rcx, 0x8(%rdi)
 3c507ef: 48 89 47 10                  	movq	%rax, 0x10(%rdi)
 3c507f3: c6 47 38 01                  	movb	$0x1, 0x38(%rdi)
 3c507f7: c5 f8 11 57 40               	vmovups	%xmm2, 0x40(%rdi)
 3c507fc: 48 c7 47 60 00 00 00 00      	movq	$0x0, 0x60(%rdi)
 3c50804: c5 f8 11 47 68               	vmovups	%xmm0, 0x68(%rdi)
 3c50809: 48 c7 87 80 00 00 00 00 00 00 00     	movq	$0x0, 0x80(%rdi)
 3c50814: c7 87 88 00 00 00 00 00 00 00	movl	$0x0, 0x88(%rdi)
 3c5081e: c5 f8 11 8f 10 0b 00 00      	vmovups	%xmm1, 0xb10(%rdi)
 3c50826: c6 87 20 0b 00 00 00         	movb	$0x0, 0xb20(%rdi)
 3c5082d: 48 c7 87 70 0d 00 00 00 00 00 00     	movq	$0x0, 0xd70(%rdi)
 3c50838: c7 87 78 0d 00 00 02 00 00 00	movl	$0x2, 0xd78(%rdi)
 3c50842: c5 f8 11 97 80 0d 00 00      	vmovups	%xmm2, 0xd80(%rdi)
 3c5084a: 48 c7 87 a0 0d 00 00 00 00 00 00     	movq	$0x0, 0xda0(%rdi)
 3c50855: c5 f8 11 87 a8 0d 00 00      	vmovups	%xmm0, 0xda8(%rdi)
 3c5085d: 48 c7 87 c0 0d 00 00 00 00 00 00     	movq	$0x0, 0xdc0(%rdi)
 3c50868: c7 87 c8 0d 00 00 00 00 00 00	movl	$0x0, 0xdc8(%rdi)
 3c50872: c5 f8 11 97 d0 0d 00 00      	vmovups	%xmm2, 0xdd0(%rdi)
 3c5087a: 48 c7 87 f0 0d 00 00 00 00 00 00     	movq	$0x0, 0xdf0(%rdi)
 3c50885: c5 f8 11 87 f8 0d 00 00      	vmovups	%xmm0, 0xdf8(%rdi)
 3c5088d: 48 c7 87 10 0e 00 00 00 00 00 00     	movq	$0x0, 0xe10(%rdi)
 3c50898: c7 87 18 0e 00 00 00 00 00 00	movl	$0x0, 0xe18(%rdi)
 3c508a2: c7 87 20 0e 00 00 ff ff ff ff	movl	$0xffffffff, 0xe20(%rdi) # imm = 0xFFFFFFFF
 3c508ac: c6 87 24 0e 00 00 00         	movb	$0x0, 0xe24(%rdi)
 3c508b3: c7 87 30 0e 00 00 00 00 00 00	movl	$0x0, 0xe30(%rdi)
 3c508bd: c5 f8 11 97 40 0e 00 00      	vmovups	%xmm2, 0xe40(%rdi)
 3c508c5: 4c 89 e7                     	movq	%r12, %rdi
 3c508c8: e8 d3 ac d7 00               	callq	0x49cb5a0 <plt_scePthreadMutexattrInit>
 3c508cd: 4c 89 e7                     	movq	%r12, %rdi
 3c508d0: be 02 00 00 00               	movl	$0x2, %esi
 3c508d5: e8 d6 ac d7 00               	callq	0x49cb5b0 <plt_scePthreadMutexattrSettype>
 3c508da: 4c 89 ff                     	movq	%r15, %rdi
 3c508dd: 4c 89 e6                     	movq	%r12, %rsi
 3c508e0: 31 d2                        	xorl	%edx, %edx
 3c508e2: e8 99 ac d7 00               	callq	0x49cb580 <plt_scePthreadMutexInit>
 3c508e7: 4c 89 e7                     	movq	%r12, %rdi
 3c508ea: e8 a1 ac d7 00               	callq	0x49cb590 <plt_scePthreadMutexattrDestroy>
 3c508ef: 4c 8d 65 c8                  	leaq	-0x38(%rbp), %r12
 3c508f3: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c508f7: c5 f8 11 83 64 11 00 00      	vmovups	%xmm0, 0x1164(%rbx)
 3c508ff: c5 f8 11 83 58 11 00 00      	vmovups	%xmm0, 0x1158(%rbx)
 3c50907: c5 f8 11 83 78 11 00 00      	vmovups	%xmm0, 0x1178(%rbx)
 3c5090f: c5 f8 11 83 84 11 00 00      	vmovups	%xmm0, 0x1184(%rbx)
 3c50917: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c5091b: 4c 8d bb 98 11 00 00         	leaq	0x1198(%rbx), %r15
 3c50922: c5 f8 11 83 a0 11 00 00      	vmovups	%xmm0, 0x11a0(%rbx)
 3c5092a: 4c 89 e7                     	movq	%r12, %rdi
 3c5092d: e8 6e ac d7 00               	callq	0x49cb5a0 <plt_scePthreadMutexattrInit>
 3c50932: 4c 89 e7                     	movq	%r12, %rdi
 3c50935: be 02 00 00 00               	movl	$0x2, %esi
 3c5093a: e8 71 ac d7 00               	callq	0x49cb5b0 <plt_scePthreadMutexattrSettype>
 3c5093f: 4c 89 ff                     	movq	%r15, %rdi
 3c50942: 4c 89 e6                     	movq	%r12, %rsi
 3c50945: 31 d2                        	xorl	%edx, %edx
 3c50947: e8 34 ac d7 00               	callq	0x49cb580 <plt_scePthreadMutexInit>
 3c5094c: 4c 89 e7                     	movq	%r12, %rdi
 3c5094f: e8 3c ac d7 00               	callq	0x49cb590 <plt_scePthreadMutexattrDestroy>
 3c50954: ba 80 0a 00 00               	movl	$0xa80, %edx            # imm = 0xA80
 3c50959: 4c 89 f7                     	movq	%r14, %rdi
 3c5095c: 31 f6                        	xorl	%esi, %esi
 3c5095e: e8 bd aa d7 00               	callq	0x49cb420 <plt_memset>
 3c50963: 48 89 df                     	movq	%rbx, %rdi
 3c50966: e8 25 00 00 00               	callq	0x3c50990 <module_init+0x3c50970>
 3c5096b: 49 8b 45 00                  	movq	(%r13), %rax
 3c5096f: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
 3c50973: 75 0f                        	jne	0x3c50984 <module_init+0x3c50964>
 3c50975: 48 83 c4 18                  	addq	$0x18, %rsp
 3c50979: 5b                           	popq	%rbx
 3c5097a: 41 5c                        	popq	%r12
 3c5097c: 41 5d                        	popq	%r13
 3c5097e: 41 5e                        	popq	%r14
 3c50980: 41 5f                        	popq	%r15
 3c50982: 5d                           	popq	%rbp
 3c50983: c3                           	retq
 3c50984: e8 27 aa d7 00               	callq	0x49cb3b0 <plt___stack_chk_fail>
 3c50989: 0f 0b                        	ud2
 3c5098b: 90                           	nop
 3c5098c: 90                           	nop
 3c5098d: 90                           	nop
 3c5098e: 90                           	nop
 3c5098f: 90                           	nop
 3c50990: 55                           	pushq	%rbp
 3c50991: 48 89 e5                     	movq	%rsp, %rbp
 3c50994: 41 57                        	pushq	%r15
 3c50996: 41 56                        	pushq	%r14
 3c50998: 41 55                        	pushq	%r13
 3c5099a: 41 54                        	pushq	%r12
 3c5099c: 53                           	pushq	%rbx
 3c5099d: 48 81 ec 18 01 00 00         	subq	$0x118, %rsp            # imm = 0x118
 3c509a4: 4c 8b 25 9d 26 92 02         	movq	0x292269d(%rip), %r12   # 0x6573048 <plt_log10+0x1ba2af8>
 3c509ab: 49 89 ff                     	movq	%rdi, %r15
 3c509ae: bf ed 00 00 00               	movl	$0xed, %edi
 3c509b3: 49 8b 04 24                  	movq	(%r12), %rax
 3c509b7: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
 3c509bb: e8 20 ab d7 00               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
 3c509c0: 85 c0                        	testl	%eax, %eax
 3c509c2: 41 89 87 10 0b 00 00         	movl	%eax, 0xb10(%r15)
 3c509c9: 0f 88 da 05 00 00            	js	0x3c50fa9 <module_init+0x3c50f89>
 3c509cf: bf a4 00 00 00               	movl	$0xa4, %edi
 3c509d4: e8 07 ab d7 00               	callq	0x49cb4e0 <plt_sceSysmoduleLoadModule>
 3c509d9: 85 c0                        	testl	%eax, %eax
 3c509db: 0f 85 c8 05 00 00            	jne	0x3c50fa9 <module_init+0x3c50f89>
 3c509e1: e8 6a c0 d7 00               	callq	0x49cca50 <plt_sceCommonDialogInitialize>
 3c509e6: bf ff 00 00 00               	movl	$0xff, %edi
 3c509eb: 31 f6                        	xorl	%esi, %esi
 3c509ed: 31 d2                        	xorl	%edx, %edx
 3c509ef: 31 c9                        	xorl	%ecx, %ecx
 3c509f1: e8 2a ed d7 00               	callq	0x49cf720 <plt_sceCameraOpen>
 3c509f6: 49 8d 9f 80 0d 00 00         	leaq	0xd80(%r15), %rbx
 3c509fd: 4c 8d b5 10 ff ff ff         	leaq	-0xf0(%rbp), %r14
 3c50a04: 4c 8d ad d0 fe ff ff         	leaq	-0x130(%rbp), %r13
 3c50a0b: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50a12: 41 89 87 14 0b 00 00         	movl	%eax, 0xb14(%r15)
 3c50a19: c7 85 10 ff ff ff 00 00 00 00	movl	$0x0, -0xf0(%rbp)
 3c50a23: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50a2d: 31 d2                        	xorl	%edx, %edx
 3c50a2f: 48 89 df                     	movq	%rbx, %rdi
 3c50a32: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50a39: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50a40: e8 cb 4e 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50a45: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50a4c: 48 89 df                     	movq	%rbx, %rdi
 3c50a4f: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50a56: 31 d2                        	xorl	%edx, %edx
 3c50a58: c7 85 10 ff ff ff 01 00 00 00	movl	$0x1, -0xf0(%rbp)
 3c50a62: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50a6c: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50a73: e8 98 4e 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50a78: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50a7f: 48 89 df                     	movq	%rbx, %rdi
 3c50a82: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50a89: 31 d2                        	xorl	%edx, %edx
 3c50a8b: c7 85 10 ff ff ff 02 00 00 00	movl	$0x2, -0xf0(%rbp)
 3c50a95: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50a9f: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50aa6: e8 65 4e 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50aab: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50ab2: 48 89 df                     	movq	%rbx, %rdi
 3c50ab5: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50abc: 31 d2                        	xorl	%edx, %edx
 3c50abe: c7 85 10 ff ff ff 03 00 00 00	movl	$0x3, -0xf0(%rbp)
 3c50ac8: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50ad2: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50ad9: e8 32 4e 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50ade: 49 8d 9f d0 0d 00 00         	leaq	0xdd0(%r15), %rbx
 3c50ae5: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50aec: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50af3: c7 85 10 ff ff ff 00 00 00 00	movl	$0x0, -0xf0(%rbp)
 3c50afd: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50b07: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50b0e: 31 d2                        	xorl	%edx, %edx
 3c50b10: 48 89 df                     	movq	%rbx, %rdi
 3c50b13: e8 f8 4d 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50b18: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50b1f: 48 89 df                     	movq	%rbx, %rdi
 3c50b22: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50b29: 31 d2                        	xorl	%edx, %edx
 3c50b2b: c7 85 10 ff ff ff 01 00 00 00	movl	$0x1, -0xf0(%rbp)
 3c50b35: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50b3f: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50b46: e8 c5 4d 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50b4b: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50b52: 48 89 df                     	movq	%rbx, %rdi
 3c50b55: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50b5c: 31 d2                        	xorl	%edx, %edx
 3c50b5e: c7 85 10 ff ff ff 02 00 00 00	movl	$0x2, -0xf0(%rbp)
 3c50b68: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50b72: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50b79: e8 92 4d 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50b7e: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50b85: 48 89 df                     	movq	%rbx, %rdi
 3c50b88: 4c 89 b5 68 ff ff ff         	movq	%r14, -0x98(%rbp)
 3c50b8f: 31 d2                        	xorl	%edx, %edx
 3c50b91: c7 85 10 ff ff ff 03 00 00 00	movl	$0x3, -0xf0(%rbp)
 3c50b9b: c7 85 d0 fe ff ff 00 00 00 00	movl	$0x0, -0x130(%rbp)
 3c50ba5: 4c 89 ad 70 ff ff ff         	movq	%r13, -0x90(%rbp)
 3c50bac: e8 5f 4d 00 00               	callq	0x3c55910 <module_init+0x3c558f0>
 3c50bb1: c5 f8 10 05 d7 f6 1b 01      	vmovups	0x11bf6d7(%rip), %xmm0  # 0x4e10290 <plt_log10+0x43fd40>
 3c50bb9: 48 b8 40 00 00 00 64 00 00 00	movabsq	$0x6400000040, %rax     # imm = 0x6400000040
 3c50bc3: 48 8d bd 10 ff ff ff         	leaq	-0xf0(%rbp), %rdi
 3c50bca: 48 8d b5 d0 fe ff ff         	leaq	-0x130(%rbp), %rsi
 3c50bd1: c5 f0 57 c9                  	vxorps	%xmm1, %xmm1, %xmm1
 3c50bd5: c5 fc 11 8d 30 ff ff ff      	vmovups	%ymm1, -0xd0(%rbp)
 3c50bdd: c5 fc 11 8d 18 ff ff ff      	vmovups	%ymm1, -0xe8(%rbp)
 3c50be5: c5 fc 11 8d f0 fe ff ff      	vmovups	%ymm1, -0x110(%rbp)
 3c50bed: c5 fc 11 8d d4 fe ff ff      	vmovups	%ymm1, -0x12c(%rbp)
 3c50bf5: c7 85 d0 fe ff ff 40 00 00 00	movl	$0x40, -0x130(%rbp)
 3c50bff: 48 89 85 10 ff ff ff         	movq	%rax, -0xf0(%rbp)
 3c50c06: c5 f8 11 85 30 ff ff ff      	vmovups	%xmm0, -0xd0(%rbp)
 3c50c0e: e8 bd eb d7 00               	callq	0x49cf7d0 <plt_sceVrTrackerQueryMemory>
 3c50c13: 85 c0                        	testl	%eax, %eax
 3c50c15: 0f 85 8e 03 00 00            	jne	0x3c50fa9 <module_init+0x3c50f89>
 3c50c1b: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c50c1f: c4 c1 7c 11 87 b0 0e 00 00   	vmovups	%ymm0, 0xeb0(%r15)
 3c50c28: c4 c1 7c 11 87 98 0e 00 00   	vmovups	%ymm0, 0xe98(%r15)
 3c50c31: c4 c1 7c 11 87 78 0e 00 00   	vmovups	%ymm0, 0xe78(%r15)
 3c50c3a: c4 c1 7c 11 87 58 0e 00 00   	vmovups	%ymm0, 0xe58(%r15)
 3c50c43: 41 c7 87 50 0e 00 00 80 00 00 00     	movl	$0x80, 0xe50(%r15)
 3c50c4e: 48 8d 85 30 ff ff ff         	leaq	-0xd0(%rbp), %rax
 3c50c55: 48 8d bd 68 ff ff ff         	leaq	-0x98(%rbp), %rdi
 3c50c5c: 4d 8d b7 50 0e 00 00         	leaq	0xe50(%r15), %r14
 3c50c63: 8b 8d 14 ff ff ff            	movl	-0xec(%rbp), %ecx
 3c50c69: 41 89 8f 54 0e 00 00         	movl	%ecx, 0xe54(%r15)
 3c50c70: b9 01 00 00 00               	movl	$0x1, %ecx
 3c50c75: c5 fc 10 00                  	vmovups	(%rax), %ymm0
 3c50c79: c4 c1 7c 11 87 78 0e 00 00   	vmovups	%ymm0, 0xe78(%r15)
 3c50c82: 8b b5 d4 fe ff ff            	movl	-0x12c(%rbp), %esi
 3c50c88: 8b 95 d8 fe ff ff            	movl	-0x128(%rbp), %edx
 3c50c8e: e8 9d b4 39 fd               	callq	0xfec130 <module_init+0xfec110>
 3c50c93: c5 f8 10 85 68 ff ff ff      	vmovups	-0x98(%rbp), %xmm0
 3c50c9b: c5 f8 10 8d 74 ff ff ff      	vmovups	-0x8c(%rbp), %xmm1
 3c50ca3: 48 8d bd 68 ff ff ff         	leaq	-0x98(%rbp), %rdi
 3c50caa: b9 02 00 00 00               	movl	$0x2, %ecx
 3c50caf: c4 c1 78 11 8f 64 11 00 00   	vmovups	%xmm1, 0x1164(%r15)
 3c50cb8: c4 c1 78 11 87 58 11 00 00   	vmovups	%xmm0, 0x1158(%r15)
 3c50cc1: 49 8b 87 58 11 00 00         	movq	0x1158(%r15), %rax
 3c50cc8: 49 89 87 98 0e 00 00         	movq	%rax, 0xe98(%r15)
 3c50ccf: 8b 85 d4 fe ff ff            	movl	-0x12c(%rbp), %eax
 3c50cd5: 41 89 87 a0 0e 00 00         	movl	%eax, 0xea0(%r15)
 3c50cdc: 8b 85 d8 fe ff ff            	movl	-0x128(%rbp), %eax
 3c50ce2: 41 89 87 a4 0e 00 00         	movl	%eax, 0xea4(%r15)
 3c50ce9: 8b b5 dc fe ff ff            	movl	-0x124(%rbp), %esi
 3c50cef: 8b 95 e0 fe ff ff            	movl	-0x120(%rbp), %edx
 3c50cf5: e8 36 b4 39 fd               	callq	0xfec130 <module_init+0xfec110>
 3c50cfa: c5 f8 10 85 68 ff ff ff      	vmovups	-0x98(%rbp), %xmm0
 3c50d02: c5 f8 10 8d 74 ff ff ff      	vmovups	-0x8c(%rbp), %xmm1
 3c50d0a: c4 c1 78 11 8f 84 11 00 00   	vmovups	%xmm1, 0x1184(%r15)
 3c50d13: c4 c1 78 11 87 78 11 00 00   	vmovups	%xmm0, 0x1178(%r15)
 3c50d1c: 49 8b 87 78 11 00 00         	movq	0x1178(%r15), %rax
 3c50d23: 49 89 87 a8 0e 00 00         	movq	%rax, 0xea8(%r15)
 3c50d2a: 8b 85 dc fe ff ff            	movl	-0x124(%rbp), %eax
 3c50d30: 41 89 87 b0 0e 00 00         	movl	%eax, 0xeb0(%r15)
 3c50d37: 8b 85 e0 fe ff ff            	movl	-0x120(%rbp), %eax
 3c50d3d: 41 89 87 b4 0e 00 00         	movl	%eax, 0xeb4(%r15)
 3c50d44: 8b b5 e4 fe ff ff            	movl	-0x11c(%rbp), %esi
 3c50d4a: 8b bd e8 fe ff ff            	movl	-0x118(%rbp), %edi
 3c50d50: e8 ab e9 d7 00               	callq	0x49cf700 <plt_memalign>
 3c50d55: 49 89 87 b8 0e 00 00         	movq	%rax, 0xeb8(%r15)
 3c50d5c: 48 85 c0                     	testq	%rax, %rax
 3c50d5f: 8b 95 e4 fe ff ff            	movl	-0x11c(%rbp), %edx
 3c50d65: 41 89 97 c0 0e 00 00         	movl	%edx, 0xec0(%r15)
 3c50d6c: 8b 8d e8 fe ff ff            	movl	-0x118(%rbp), %ecx
 3c50d72: 41 89 8f c4 0e 00 00         	movl	%ecx, 0xec4(%r15)
 3c50d79: 74 0a                        	je	0x3c50d85 <module_init+0x3c50d65>
 3c50d7b: 48 89 c7                     	movq	%rax, %rdi
 3c50d7e: 31 f6                        	xorl	%esi, %esi
 3c50d80: e8 9b a6 d7 00               	callq	0x49cb420 <plt_memset>
 3c50d85: 4c 89 f7                     	movq	%r14, %rdi
 3c50d88: 49 c7 87 c8 0e 00 00 06 00 00 00     	movq	$0x6, 0xec8(%r15)
 3c50d93: e8 28 ea d7 00               	callq	0x49cf7c0 <plt_sceVrTrackerInit>
 3c50d98: 85 c0                        	testl	%eax, %eax
 3c50d9a: 0f 85 09 02 00 00            	jne	0x3c50fa9 <module_init+0x3c50f89>
 3c50da0: 49 8d bf d4 0e 00 00         	leaq	0xed4(%r15), %rdi
 3c50da7: ba 84 02 00 00               	movl	$0x284, %edx            # imm = 0x284
 3c50dac: 31 f6                        	xorl	%esi, %esi
 3c50dae: 45 31 f6                     	xorl	%r14d, %r14d
 3c50db1: e8 6a a6 d7 00               	callq	0x49cb420 <plt_memset>
 3c50db6: 49 8d bf 30 0b 00 00         	leaq	0xb30(%r15), %rdi
 3c50dbd: ba 40 02 00 00               	movl	$0x240, %edx            # imm = 0x240
 3c50dc2: 41 c7 87 d0 0e 00 00 88 02 00 00     	movl	$0x288, 0xed0(%r15) # imm = 0x288
 3c50dcd: 31 f6                        	xorl	%esi, %esi
 3c50dcf: 41 c7 87 e0 0e 00 00 00 00 00 00     	movl	$0x0, 0xee0(%r15)
 3c50dda: e8 41 a6 d7 00               	callq	0x49cb420 <plt_memset>
 3c50ddf: 48 b8 48 02 00 00 11 00 00 00	movabsq	$0x1100000248, %rax     # imm = 0x1100000248
 3c50de9: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c50ded: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50df4: 49 89 87 28 0b 00 00         	movq	%rax, 0xb28(%r15)
 3c50dfb: 48 b8 68 00 00 00 05 00 00 00	movabsq	$0x500000068, %rax      # imm = 0x500000068
 3c50e05: c5 fc 11 45 b0               	vmovups	%ymm0, -0x50(%rbp)
 3c50e0a: c5 fc 11 45 90               	vmovups	%ymm0, -0x70(%rbp)
 3c50e0f: c5 fc 11 85 70 ff ff ff      	vmovups	%ymm0, -0x90(%rbp)
 3c50e17: 48 89 85 68 ff ff ff         	movq	%rax, -0x98(%rbp)
 3c50e1e: 41 8b bf 14 0b 00 00         	movl	0xb14(%r15), %edi
 3c50e25: e8 06 e9 d7 00               	callq	0x49cf730 <plt_sceCameraSetConfig>
 3c50e2a: 48 b8 10 00 00 00 01 00 00 00	movabsq	$0x100000010, %rax      # imm = 0x100000010
 3c50e34: 48 8d b5 c0 fe ff ff         	leaq	-0x140(%rbp), %rsi
 3c50e3b: 48 89 85 c0 fe ff ff         	movq	%rax, -0x140(%rbp)
 3c50e42: 48 c7 85 c8 fe ff ff 00 00 00 00     	movq	$0x0, -0x138(%rbp)
 3c50e4d: 41 8b bf 14 0b 00 00         	movl	0xb14(%r15), %edi
 3c50e54: e8 e7 e8 d7 00               	callq	0x49cf740 <plt_sceCameraSetVideoSync>
 3c50e59: 48 b8 18 00 00 00 0f 00 00 00	movabsq	$0xf00000018, %rax      # imm = 0xF00000018
 3c50e63: c5 f8 57 c0                  	vxorps	%xmm0, %xmm0, %xmm0
 3c50e67: c5 f8 11 85 50 ff ff ff      	vmovups	%xmm0, -0xb0(%rbp)
 3c50e6f: 48 c7 85 60 ff ff ff 00 00 00 00     	movq	$0x0, -0xa0(%rbp)
 3c50e7a: 48 8d b5 50 ff ff ff         	leaq	-0xb0(%rbp), %rsi
 3c50e81: 48 89 85 50 ff ff ff         	movq	%rax, -0xb0(%rbp)
 3c50e88: c7 85 58 ff ff ff 0f 00 00 00	movl	$0xf, -0xa8(%rbp)
 3c50e92: 41 8b bf 14 0b 00 00         	movl	0xb14(%r15), %edi
 3c50e99: e8 b2 e8 d7 00               	callq	0x49cf750 <plt_sceCameraStart>
 3c50e9e: 49 8d bf 28 0e 00 00         	leaq	0xe28(%r15), %rdi
 3c50ea5: 48 8d 15 74 04 00 00         	leaq	0x474(%rip), %rdx       # 0x3c51320 <module_init+0x3c51300>
 3c50eac: 4c 8d 05 d1 f6 1b 01         	leaq	0x11bf6d1(%rip), %r8    # 0x4e10584 <plt_log10+0x440034>
 3c50eb3: 4c 89 f9                     	movq	%r15, %rcx
 3c50eb6: 31 f6                        	xorl	%esi, %esi
 3c50eb8: e8 43 d4 d7 00               	callq	0x49ce300 <plt_scePthreadCreate>
 3c50ebd: 49 8b bf 28 0e 00 00         	movq	0xe28(%r15), %rdi
 3c50ec4: be 00 01 00 00               	movl	$0x100, %esi            # imm = 0x100
 3c50ec9: e8 a2 e8 d7 00               	callq	0x49cf770 <plt_scePthreadSetprio>
 3c50ece: 31 ff                        	xorl	%edi, %edi
 3c50ed0: e8 6b bb d7 00               	callq	0x49cca40 <plt_sceCameraIsAttached>
 3c50ed5: 48 8d bd 68 ff ff ff         	leaq	-0x98(%rbp), %rdi
 3c50edc: be 30 00 00 00               	movl	$0x30, %esi
 3c50ee1: 41 89 87 20 0e 00 00         	movl	%eax, 0xe20(%r15)
 3c50ee8: 49 c7 87 18 0b 00 00 00 00 00 00     	movq	$0x0, 0xb18(%r15)
 3c50ef3: 48 c7 85 68 ff ff ff 00 00 00 00     	movq	$0x0, -0x98(%rbp)
 3c50efe: c7 85 70 ff ff ff 00 00 00 00	movl	$0x0, -0x90(%rbp)
 3c50f08: e8 03 f2 3b fc               	callq	0x10110 <module_init+0x100f0>
 3c50f0d: 48 89 c3                     	movq	%rax, %rbx
 3c50f10: 48 8d 0d 31 21 29 02         	leaq	0x2292131(%rip), %rcx   # 0x5ee3048 <plt_log10+0x1512af8>
 3c50f17: 48 89 0b                     	movq	%rcx, (%rbx)
 3c50f1a: e8 81 50 3c fd               	callq	0x1015fa0 <module_init+0x1015f80>
 3c50f1f: 48 8d 15 3a f4 81 02         	leaq	0x281f43a(%rip), %rdx   # 0x6470360 <plt_log10+0x1a9fe10>
 3c50f26: 48 8d 0d 83 04 00 00         	leaq	0x483(%rip), %rcx       # 0x3c513b0 <module_init+0x3c51390>
 3c50f2d: 48 89 43 10                  	movq	%rax, 0x10(%rbx)
 3c50f31: 48 89 13                     	movq	%rdx, (%rbx)
 3c50f34: 4c 89 7b 18                  	movq	%r15, 0x18(%rbx)
 3c50f38: 48 89 4b 20                  	movq	%rcx, 0x20(%rbx)
 3c50f3c: 48 c7 43 28 00 00 00 00      	movq	$0x0, 0x28(%rbx)
 3c50f44: 83 bd 70 ff ff ff 00         	cmpl	$0x0, -0x90(%rbp)
 3c50f4b: 74 20                        	je	0x3c50f6d <module_init+0x3c50f4d>
 3c50f4d: 48 83 bd 68 ff ff ff 00      	cmpq	$0x0, -0x98(%rbp)
 3c50f55: 74 16                        	je	0x3c50f6d <module_init+0x3c50f4d>
 3c50f57: 48 8d 3d ea 2f f7 02         	leaq	0x2f72fea(%rip), %rdi   # 0x6bc3f48
 3c50f5e: 48 8d b5 68 ff ff ff         	leaq	-0x98(%rbp), %rsi
 3c50f65: e8 16 b1 3c fc               	callq	0x1c080 <module_init+0x1c060>
 3c50f6a: 49 89 c6                     	movq	%rax, %r14
 3c50f6d: 48 8d bd 68 ff ff ff         	leaq	-0x98(%rbp), %rdi
 3c50f74: e8 d7 d8 f3 fc               	callq	0xb8e850 <module_init+0xb8e830>
 3c50f79: 4d 89 b7 70 0d 00 00         	movq	%r14, 0xd70(%r15)
 3c50f80: e8 4b 50 3c fd               	callq	0x1015fd0 <module_init+0x1015fb0>
 3c50f85: 8a 0d 15 9d e5 02            	movb	0x2e59d15(%rip), %cl    # 0x6aaaca0
 3c50f8b: 48 89 c3                     	movq	%rax, %rbx
 3c50f8e: 84 c9                        	testb	%cl, %cl
 3c50f90: 74 3a                        	je	0x3c50fcc <module_init+0x3c50fac>
 3c50f92: 48 8b 35 ff 9c e5 02         	movq	0x2e59cff(%rip), %rsi   # 0x6aaac98
 3c50f99: 48 8b 03                     	movq	(%rbx), %rax
 3c50f9c: 49 83 c7 08                  	addq	$0x8, %r15
 3c50fa0: 48 89 df                     	movq	%rbx, %rdi
 3c50fa3: 4c 89 fa                     	movq	%r15, %rdx
 3c50fa6: ff 50 20                     	callq	*0x20(%rax)
 3c50fa9: 49 8b 04 24                  	movq	(%r12), %rax
 3c50fad: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
 3c50fb1: 75 12                        	jne	0x3c50fc5 <module_init+0x3c50fa5>
 3c50fb3: 48 81 c4 18 01 00 00         	addq	$0x118, %rsp            # imm = 0x118
 3c50fba: 5b                           	popq	%rbx
 3c50fbb: 41 5c                        	popq	%r12
 3c50fbd: 41 5d                        	popq	%r13
 3c50fbf: 41 5e                        	popq	%r14
 3c50fc1: 41 5f                        	popq	%r15
 3c50fc3: 5d                           	popq	%rbp
 3c50fc4: c3                           	retq
 3c50fc5: e8 e6 a3 d7 00               	callq	0x49cb3b0 <plt___stack_chk_fail>
 3c50fca: 0f 0b                        	ud2
 3c50fcc: 48 8d 3d cd 9c e5 02         	leaq	0x2e59ccd(%rip), %rdi   # 0x6aaaca0
 3c50fd3: e8 a8 a3 d7 00               	callq	0x49cb380 <plt___cxa_guard_acquire>
 3c50fd8: 85 c0                        	testl	%eax, %eax
 3c50fda: 74 b6                        	je	0x3c50f92 <module_init+0x3c50f72>
 3c50fdc: 48 8d 3d b5 9c e5 02         	leaq	0x2e59cb5(%rip), %rdi   # 0x6aaac98
 3c50fe3: 48 8d 35 66 f6 1b 01         	leaq	0x11bf666(%rip), %rsi   # 0x4e10650 <plt_log10+0x440100>
 3c50fea: ba 01 00 00 00               	movl	$0x1, %edx
 3c50fef: e8 4c a0 5a fd               	callq	0x11fb040 <module_init+0x11fb020>
 3c50ff4: 48 8d 3d a5 9c e5 02         	leaq	0x2e59ca5(%rip), %rdi   # 0x6aaaca0
 3c50ffb: e8 90 a3 d7 00               	callq	0x49cb390 <plt___cxa_guard_release>
 3c51000: eb 90                        	jmp	0x3c50f92 <module_init+0x3c50f72>
 3c51002: 90                           	nop
 3c51003: 90                           	nop
 3c51004: 90                           	nop
 3c51005: 90                           	nop
 3c51006: 90                           	nop
 3c51007: 90                           	nop
 3c51008: 90                           	nop
 3c51009: 90                           	nop
 3c5100a: 90                           	nop
 3c5100b: 90                           	nop
 3c5100c: 90                           	nop
 3c5100d: 90                           	nop
 3c5100e: 90                           	nop
 3c5100f: 90                           	nop
 3c51010: 55                           	pushq	%rbp
 3c51011: 48 89 e5                     	movq	%rsp, %rbp
 3c51014: 41 57                        	pushq	%r15
 3c51016: 41 56                        	pushq	%r14
 3c51018: 41 54                        	pushq	%r12
 3c5101a: 53                           	pushq	%rbx
 3c5101b: 48 8d 05 2e f1 81 02         	leaq	0x281f12e(%rip), %rax   # 0x6470150 <plt_log10+0x1a9fc00>
 3c51022: 48 8d 15 ef f1 81 02         	leaq	0x281f1ef(%rip), %rdx   # 0x6470218 <plt_log10+0x1a9fcc8>
 3c51029: 48 8d 0d 38 f2 81 02         	leaq	0x281f238(%rip), %rcx   # 0x6470268 <plt_log10+0x1a9fd18>
 3c51030: 48 89 fb                     	movq	%rdi, %rbx
 3c51033: 4c 8d 77 10                  	leaq	0x10(%rdi), %r14
 3c51037: 48 89 07                     	movq	%rax, (%rdi)
 3c5103a: 48 89 57 08                  	movq	%rdx, 0x8(%rdi)
 3c5103e: 48 89 4f 10                  	movq	%rcx, 0x10(%rdi)
 3c51042: e8 19 01 00 00               	callq	0x3c51160 <module_init+0x3c51140>
 3c51047: 48 8d bb 98 11 00 00         	leaq	0x1198(%rbx), %rdi
 3c5104e: e8 3d a4 d7 00               	callq	0x49cb490 <plt_scePthreadMutexDestroy>
 3c51053: 48 8d bb 38 0e 00 00         	leaq	0xe38(%rbx), %rdi
 3c5105a: e8 31 a4 d7 00               	callq	0x49cb490 <plt_scePthreadMutexDestroy>
 3c5105f: 48 8d bb d0 0d 00 00         	leaq	0xdd0(%rbx), %rdi
 3c51066: e8 95 32 00 00               	callq	0x3c54300 <module_init+0x3c542e0>
 3c5106b: 48 8d bb 80 0d 00 00         	leaq	0xd80(%rbx), %rdi
 3c51072: e8 89 32 00 00               	callq	0x3c54300 <module_init+0x3c542e0>
 3c51077: c7 83 88 00 00 00 00 00 00 00	movl	$0x0, 0x88(%rbx)
 3c51081: 4c 8b bb 80 00 00 00         	movq	0x80(%rbx), %r15
 3c51088: 4d 85 ff                     	testq	%r15, %r15
 3c5108b: 0f 84 86 00 00 00            	je	0x3c51117 <module_init+0x3c510f7>
 3c51091: 48 8d 05 50 75 db 02         	leaq	0x2db7550(%rip), %rax   # 0x6a085e8
 3c51098: 4c 8b 20                     	movq	(%rax), %r12
 3c5109b: 4d 85 e4                     	testq	%r12, %r12
 3c5109e: 74 5a                        	je	0x3c510fa <module_init+0x3c510da>
 3c510a0: 66 45 85 ff                  	testw	%r15w, %r15w
 3c510a4: 74 66                        	je	0x3c5110c <module_init+0x3c510ec>
 3c510a6: 48 8d 05 2f 75 db 02         	leaq	0x2db752f(%rip), %rax   # 0x6a085dc
 3c510ad: 8b 38                        	movl	(%rax), %edi
 3c510af: 85 ff                        	testl	%edi, %edi
 3c510b1: 74 59                        	je	0x3c5110c <module_init+0x3c510ec>
 3c510b3: e8 b8 a3 d7 00               	callq	0x49cb470 <plt_scePthreadGetspecific>
 3c510b8: 48 85 c0                     	testq	%rax, %rax
 3c510bb: 74 4f                        	je	0x3c5110c <module_init+0x3c510ec>
 3c510bd: 4c 89 ff                     	movq	%r15, %rdi
 3c510c0: 48 81 e7 00 00 ff ff         	andq	$-0x10000, %rdi         # imm = 0xFFFF0000
 3c510c7: 80 7f 03 e3                  	cmpb	$-0x1d, 0x3(%rdi)
 3c510cb: 75 3f                        	jne	0x3c5110c <module_init+0x3c510ec>
 3c510cd: 0f b6 77 02                  	movzbl	0x2(%rdi), %esi
 3c510d1: 48 c1 e6 05                  	shlq	$0x5, %rsi
 3c510d5: 8b 54 30 08                  	movl	0x8(%rax,%rsi), %edx
 3c510d9: 4c 8d 44 30 08               	leaq	0x8(%rax,%rsi), %r8
 3c510de: 48 8d 0c 30                  	leaq	(%rax,%rsi), %rcx
 3c510e2: 83 fa 3f                     	cmpl	$0x3f, %edx
 3c510e5: 77 1d                        	ja	0x3c51104 <module_init+0x3c510e4>
 3c510e7: 0f b7 3f                     	movzwl	(%rdi), %edi
 3c510ea: 0f af d7                     	imull	%edi, %edx
 3c510ed: 81 fa ff ff 00 00            	cmpl	$0xffff, %edx           # imm = 0xFFFF
 3c510f3: 77 0f                        	ja	0x3c51104 <module_init+0x3c510e4>
 3c510f5: 48 8b 01                     	movq	(%rcx), %rax
 3c510f8: eb 53                        	jmp	0x3c5114d <module_init+0x3c5112d>
 3c510fa: 4c 89 ff                     	movq	%r15, %rdi
 3c510fd: e8 ce 77 41 fd               	callq	0x10688d0 <module_init+0x10688b0>
 3c51102: eb 13                        	jmp	0x3c51117 <module_init+0x3c510f7>
 3c51104: 48 83 7c 30 10 00            	cmpq	$0x0, 0x10(%rax,%rsi)
 3c5110a: 74 27                        	je	0x3c51133 <module_init+0x3c51113>
 3c5110c: 4c 89 e7                     	movq	%r12, %rdi
 3c5110f: 4c 89 fe                     	movq	%r15, %rsi
 3c51112: e8 c9 a5 40 fd               	callq	0x105b6e0 <module_init+0x105b6c0>
 3c51117: 48 83 c3 40                  	addq	$0x40, %rbx
 3c5111b: 48 89 df                     	movq	%rbx, %rdi
 3c5111e: e8 fd 33 00 00               	callq	0x3c54520 <module_init+0x3c54500>
 3c51123: 4c 89 f7                     	movq	%r14, %rdi
 3c51126: 5b                           	popq	%rbx
 3c51127: 41 5c                        	popq	%r12
 3c51129: 41 5e                        	popq	%r14
 3c5112b: 41 5f                        	popq	%r15
 3c5112d: 5d                           	popq	%rbp
 3c5112e: e9 1d 61 8d ff               	jmp	0x3527250 <module_init+0x3527230>
 3c51133: 8b 51 08                     	movl	0x8(%rcx), %edx
 3c51136: 48 8d 44 30 10               	leaq	0x10(%rax,%rsi), %rax
 3c5113b: 89 50 08                     	movl	%edx, 0x8(%rax)
 3c5113e: 48 8b 11                     	movq	(%rcx), %rdx
 3c51141: 48 89 10                     	movq	%rdx, (%rax)
 3c51144: 31 c0                        	xorl	%eax, %eax
 3c51146: 41 c7 00 00 00 00 00         	movl	$0x0, (%r8)
 3c5114d: 49 89 07                     	movq	%rax, (%r15)
 3c51150: 49 c7 47 08 00 00 00 00      	movq	$0x0, 0x8(%r15)
 3c51158: 4c 89 39                     	movq	%r15, (%rcx)
 3c5115b: 41 ff 00                     	incl	(%r8)
 3c5115e: eb b7                        	jmp	0x3c51117 <module_init+0x3c510f7>
 3c51160: 55                           	pushq	%rbp
 3c51161: 48 89 e5                     	movq	%rsp, %rbp
 3c51164: 41 56                        	pushq	%r14
 3c51166: 53                           	pushq	%rbx
 3c51167: 83 bf 10 0b 00 00 ff         	cmpl	$-0x1, 0xb10(%rdi)
 3c5116e: 0f 84 9c 00 00 00            	je	0x3c51210 <module_init+0x3c511f0>
 3c51174: 48 89 fb                     	movq	%rdi, %rbx
 3c51177: e8 54 4e 3c fd               	callq	0x1015fd0 <module_init+0x1015fb0>
 3c5117c: 8a 0d 1e 9b e5 02            	movb	0x2e59b1e(%rip), %cl    # 0x6aaaca0
 3c51182: 49 89 c6                     	movq	%rax, %r14
 3c51185: 84 c9                        	testb	%cl, %cl
 3c51187: 0f 84 e8 00 00 00            	je	0x3c51275 <module_init+0x3c51255>
 3c5118d: 48 8b 35 04 9b e5 02         	movq	0x2e59b04(%rip), %rsi   # 0x6aaac98
 3c51194: 49 8b 06                     	movq	(%r14), %rax
 3c51197: 48 8d 53 08                  	leaq	0x8(%rbx), %rdx
 3c5119b: 4c 89 f7                     	movq	%r14, %rdi
 3c5119e: ff 50 28                     	callq	*0x28(%rax)
 3c511a1: 48 8b b3 70 0d 00 00         	movq	0xd70(%rbx), %rsi
 3c511a8: 48 85 f6                     	testq	%rsi, %rsi
 3c511ab: 74 0c                        	je	0x3c511b9 <module_init+0x3c51199>
 3c511ad: 48 8d 3d 94 2d f7 02         	leaq	0x2f72d94(%rip), %rdi   # 0x6bc3f48
 3c511b4: e8 c7 f3 41 fc               	callq	0x70580 <module_init+0x70560>
 3c511b9: 4c 8d b3 38 0e 00 00         	leaq	0xe38(%rbx), %r14
 3c511c0: 4c 89 f7                     	movq	%r14, %rdi
 3c511c3: e8 d8 a2 d7 00               	callq	0x49cb4a0 <plt_scePthreadMutexLock>
 3c511c8: 85 c0                        	testl	%eax, %eax
 3c511ca: 75 05                        	jne	0x3c511d1 <module_init+0x3c511b1>
 3c511cc: e8 ef a2 d7 00               	callq	0x49cb4c0 <plt_scePthreadSelf>
 3c511d1: 4c 89 f7                     	movq	%r14, %rdi
 3c511d4: c7 83 30 0e 00 00 01 00 00 00	movl	$0x1, 0xe30(%rbx)
 3c511de: 48 c7 83 40 0e 00 00 00 00 00 00     	movq	$0x0, 0xe40(%rbx)
 3c511e9: e8 c2 a2 d7 00               	callq	0x49cb4b0 <plt_scePthreadMutexUnlock>
 3c511ee: 48 8b bb 28 0e 00 00         	movq	0xe28(%rbx), %rdi
 3c511f5: 31 f6                        	xorl	%esi, %esi
 3c511f7: e8 24 d1 d7 00               	callq	0x49ce320 <plt_scePthreadJoin>
 3c511fc: 8b bb 14 0b 00 00            	movl	0xb14(%rbx), %edi
 3c51202: e8 59 e5 d7 00               	callq	0x49cf760 <plt_sceCameraStop>
 3c51207: e8 f4 e5 d7 00               	callq	0x49cf800 <plt_sceVrTrackerTerm>
 3c5120c: 85 c0                        	testl	%eax, %eax
 3c5120e: 74 05                        	je	0x3c51215 <module_init+0x3c511f5>
 3c51210: 5b                           	popq	%rbx
 3c51211: 41 5e                        	popq	%r14
 3c51213: 5d                           	popq	%rbp
 3c51214: c3                           	retq
 3c51215: 48 8d bb 78 11 00 00         	leaq	0x1178(%rbx), %rdi
 3c5121c: e8 ff af 39 fd               	callq	0xfec220 <module_init+0xfec200>
 3c51221: 48 8d bb 58 11 00 00         	leaq	0x1158(%rbx), %rdi
 3c51228: 48 c7 83 a8 0e 00 00 00 00 00 00     	movq	$0x0, 0xea8(%rbx)
 3c51233: e8 e8 af 39 fd               	callq	0xfec220 <module_init+0xfec200>
 3c51238: 48 c7 83 98 0e 00 00 00 00 00 00     	movq	$0x0, 0xe98(%rbx)
 3c51243: 48 8b bb b8 0e 00 00         	movq	0xeb8(%rbx), %rdi
 3c5124a: e8 f1 cc d7 00               	callq	0x49cdf40 <plt_free>
 3c5124f: 48 c7 83 b8 0e 00 00 00 00 00 00     	movq	$0x0, 0xeb8(%rbx)
 3c5125a: 0f b7 bb 10 0b 00 00         	movzwl	0xb10(%rbx), %edi
 3c51261: e8 2a c7 d7 00               	callq	0x49cd990 <plt_sceSysmoduleUnloadModule>
 3c51266: c7 83 10 0b 00 00 ff ff ff ff	movl	$0xffffffff, 0xb10(%rbx) # imm = 0xFFFFFFFF
 3c51270: 5b                           	popq	%rbx
 3c51271: 41 5e                        	popq	%r14
 3c51273: 5d                           	popq	%rbp
 3c51274: c3                           	retq
 3c51275: 48 8d 3d 24 9a e5 02         	leaq	0x2e59a24(%rip), %rdi   # 0x6aaaca0
 3c5127c: e8 ff a0 d7 00               	callq	0x49cb380 <plt___cxa_guard_acquire>
 3c51281: 85 c0                        	testl	%eax, %eax
 3c51283: 0f 84 04 ff ff ff            	je	0x3c5118d <module_init+0x3c5116d>
 3c51289: 48 8d 3d 08 9a e5 02         	leaq	0x2e59a08(%rip), %rdi   # 0x6aaac98
 3c51290: 48 8d 35 b9 f3 1b 01         	leaq	0x11bf3b9(%rip), %rsi   # 0x4e10650 <plt_log10+0x440100>
 3c51297: ba 01 00 00 00               	movl	$0x1, %edx
 3c5129c: e8 9f 9d 5a fd               	callq	0x11fb040 <module_init+0x11fb020>
 3c512a1: 48 8d 3d f8 99 e5 02         	leaq	0x2e599f8(%rip), %rdi   # 0x6aaaca0
 3c512a8: e8 e3 a0 d7 00               	callq	0x49cb390 <plt___cxa_guard_release>
 3c512ad: e9 db fe ff ff               	jmp	0x3c5118d <module_init+0x3c5116d>
 3c512b2: 90                           	nop
 3c512b3: 90                           	nop
 3c512b4: 90                           	nop
 3c512b5: 90                           	nop
 3c512b6: 90                           	nop
 3c512b7: 90                           	nop
 3c512b8: 90                           	nop
 3c512b9: 90                           	nop
 3c512ba: 90                           	nop
 3c512bb: 90                           	nop
 3c512bc: 90                           	nop
 3c512bd: 90                           	nop
 3c512be: 90                           	nop
 3c512bf: 90                           	nop
 3c512c0: 55                           	pushq	%rbp
 3c512c1: 48 89 e5                     	movq	%rsp, %rbp
 3c512c4: 48 83 c7 f0                  	addq	$-0x10, %rdi
 3c512c8: 5d                           	popq	%rbp
 3c512c9: e9 42 fd ff ff               	jmp	0x3c51010 <module_init+0x3c50ff0>
 3c512ce: 90                           	nop
 3c512cf: 90                           	nop
 3c512d0: 55                           	pushq	%rbp
 3c512d1: 48 89 e5                     	movq	%rsp, %rbp
 3c512d4: 53                           	pushq	%rbx
 3c512d5: 50                           	pushq	%rax
 3c512d6: 48 89 fb                     	movq	%rdi, %rbx
 3c512d9: e8 32 fd ff ff               	callq	0x3c51010 <module_init+0x3c50ff0>
 3c512de: 48 89 df                     	movq	%rbx, %rdi
 3c512e1: 48 83 c4 08                  	addq	$0x8, %rsp
 3c512e5: 5b                           	popq	%rbx
 3c512e6: 5d                           	popq	%rbp
 3c512e7: e9 74 02 29 fd               	jmp	0xee1560 <module_init+0xee1540>
 3c512ec: 90                           	nop
 3c512ed: 90                           	nop
 3c512ee: 90                           	nop
 3c512ef: 90                           	nop
 3c512f0: 55                           	pushq	%rbp
 3c512f1: 48 89 e5                     	movq	%rsp, %rbp
 3c512f4: 53                           	pushq	%rbx
 3c512f5: 50                           	pushq	%rax
 3c512f6: 48 89 fb                     	movq	%rdi, %rbx
 3c512f9: 48 83 c3 f0                  	addq	$-0x10, %rbx
 3c512fd: 48 89 df                     	movq	%rbx, %rdi
 3c51300: e8 0b fd ff ff               	callq	0x3c51010 <module_init+0x3c50ff0>
 3c51305: 48 89 df                     	movq	%rbx, %rdi
 3c51308: 48 83 c4 08                  	addq	$0x8, %rsp
 3c5130c: 5b                           	popq	%rbx
 3c5130d: 5d                           	popq	%rbp
 3c5130e: e9 4d 02 29 fd               	jmp	0xee1560 <module_init+0xee1540>
 3c51313: 90                           	nop
 3c51314: 90                           	nop
 3c51315: 90                           	nop
 3c51316: 90                           	nop
 3c51317: 90                           	nop
 3c51318: 90                           	nop
 3c51319: 90                           	nop
 3c5131a: 90                           	nop
 3c5131b: 90                           	nop
 3c5131c: 90                           	nop
 3c5131d: 90                           	nop
 3c5131e: 90                           	nop
 3c5131f: 90                           	nop
