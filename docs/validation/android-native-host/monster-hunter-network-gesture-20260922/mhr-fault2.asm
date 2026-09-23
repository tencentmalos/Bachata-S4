
build/validation/monster-hunter-network-gesture-20260922/mhr-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
 4a513c0: e8 ab 76 54 ff               	callq	0x3f98a70 <module_init+0x3f98a50>
 4a513c5: 8b 45 bc                     	movl	-0x44(%rbp), %eax
 4a513c8: 48 8d 4d bc                  	leaq	-0x44(%rbp), %rcx
 4a513cc: ba ff ff ff ff               	movl	$0xffffffff, %edx       # imm = 0xFFFFFFFF
 4a513d1: 89 45 cc                     	movl	%eax, -0x34(%rbp)
 4a513d4: 49 8b 46 50                  	movq	0x50(%r14), %rax
 4a513d8: 0f b6 b0 fe 00 00 00         	movzbl	0xfe(%rax), %esi
 4a513df: 48 8b b8 a8 00 00 00         	movq	0xa8(%rax), %rdi
 4a513e6: c1 e6 03                     	shll	$0x3, %esi
 4a513e9: e8 82 76 54 ff               	callq	0x3f98a70 <module_init+0x3f98a50>
 4a513ee: 8b 45 bc                     	movl	-0x44(%rbp), %eax
 4a513f1: 89 45 d0                     	movl	%eax, -0x30(%rbp)
 4a513f4: c7 45 d4 da ec da 1c         	movl	$0x1cdaecda, -0x2c(%rbp) # imm = 0x1CDAECDA
 4a513fb: 49 8b 46 48                  	movq	0x48(%r14), %rax
 4a513ff: 48 85 c0                     	testq	%rax, %rax
 4a51402: 74 06                        	je	0x4a5140a <module_init+0x4a513ea>
 4a51404: 8b 40 10                     	movl	0x10(%rax), %eax
 4a51407: 89 45 d4                     	movl	%eax, -0x2c(%rbp)
 4a5140a: 48 8d 7d c0                  	leaq	-0x40(%rbp), %rdi
 4a5140e: 48 8d 4d bc                  	leaq	-0x44(%rbp), %rcx
 4a51412: be 18 00 00 00               	movl	$0x18, %esi
 4a51417: ba ff ff ff ff               	movl	$0xffffffff, %edx       # imm = 0xFFFFFFFF
 4a5141c: e8 4f 76 54 ff               	callq	0x3f98a70 <module_init+0x3f98a50>
 4a51421: 8b 45 bc                     	movl	-0x44(%rbp), %eax
 4a51424: 89 43 18                     	movl	%eax, 0x18(%rbx)
 4a51427: 48 8b 43 50                  	movq	0x50(%rbx), %rax
 4a5142b: 48 85 c0                     	testq	%rax, %rax
 4a5142e: 74 04                        	je	0x4a51434 <module_init+0x4a51414>
 4a51430: f0                           	lock
 4a51431: ff 40 08                     	incl	0x8(%rax)
 4a51434: 48 8b 43 58                  	movq	0x58(%rbx), %rax
 4a51438: 48 85 c0                     	testq	%rax, %rax
 4a5143b: 74 04                        	je	0x4a51441 <module_init+0x4a51421>
 4a5143d: f0                           	lock
 4a5143e: ff 40 08                     	incl	0x8(%rax)
 4a51441: 48 8b 4b 60                  	movq	0x60(%rbx), %rcx
 4a51445: 48 8d 43 20                  	leaq	0x20(%rbx), %rax
 4a51449: 48 85 c9                     	testq	%rcx, %rcx
 4a5144c: 74 04                        	je	0x4a51452 <module_init+0x4a51432>
 4a5144e: f0                           	lock
 4a5144f: ff 41 08                     	incl	0x8(%rcx)
 4a51452: 48 8b 00                     	movq	(%rax), %rax
 4a51455: 48 85 c0                     	testq	%rax, %rax
 4a51458: 74 04                        	je	0x4a5145e <module_init+0x4a5143e>
 4a5145a: f0                           	lock
 4a5145b: ff 40 08                     	incl	0x8(%rax)
 4a5145e: 48 8b 43 28                  	movq	0x28(%rbx), %rax
 4a51462: 48 85 c0                     	testq	%rax, %rax
 4a51465: 74 04                        	je	0x4a5146b <module_init+0x4a5144b>
 4a51467: f0                           	lock
 4a51468: ff 40 08                     	incl	0x8(%rax)
 4a5146b: 48 8b 43 30                  	movq	0x30(%rbx), %rax
 4a5146f: 48 85 c0                     	testq	%rax, %rax
 4a51472: 74 04                        	je	0x4a51478 <module_init+0x4a51458>
 4a51474: f0                           	lock
 4a51475: ff 40 08                     	incl	0x8(%rax)
 4a51478: 48 8b 43 40                  	movq	0x40(%rbx), %rax
 4a5147c: 48 85 c0                     	testq	%rax, %rax
 4a5147f: 74 04                        	je	0x4a51485 <module_init+0x4a51465>
 4a51481: f0                           	lock
 4a51482: ff 40 08                     	incl	0x8(%rax)
 4a51485: 48 8b 43 48                  	movq	0x48(%rbx), %rax
 4a51489: 48 85 c0                     	testq	%rax, %rax
 4a5148c: 74 04                        	je	0x4a51492 <module_init+0x4a51472>
 4a5148e: f0                           	lock
 4a5148f: ff 40 08                     	incl	0x8(%rax)
 4a51492: 48 8b 43 38                  	movq	0x38(%rbx), %rax
 4a51496: 48 85 c0                     	testq	%rax, %rax
 4a51499: 74 04                        	je	0x4a5149f <module_init+0x4a5147f>
 4a5149b: f0                           	lock
 4a5149c: ff 40 08                     	incl	0x8(%rax)
 4a5149f: 48 8b 43 68                  	movq	0x68(%rbx), %rax
 4a514a3: 48 85 c0                     	testq	%rax, %rax
 4a514a6: 74 04                        	je	0x4a514ac <module_init+0x4a5148c>
 4a514a8: f0                           	lock
 4a514a9: ff 40 08                     	incl	0x8(%rax)
 4a514ac: 48 8b 43 70                  	movq	0x70(%rbx), %rax
 4a514b0: 0f b7 80 06 01 00 00         	movzwl	0x106(%rax), %eax
 4a514b7: 89 83 a0 00 00 00            	movl	%eax, 0xa0(%rbx)
 4a514bd: 49 8b 04 24                  	movq	(%r12), %rax
 4a514c1: 48 3b 45 d8                  	cmpq	-0x28(%rbp), %rax
 4a514c5: 75 0d                        	jne	0x4a514d4 <module_init+0x4a514b4>
 4a514c7: 48 83 c4 30                  	addq	$0x30, %rsp
 4a514cb: 5b                           	popq	%rbx
 4a514cc: 41 5c                        	popq	%r12
 4a514ce: 41 5e                        	popq	%r14
 4a514d0: 41 5f                        	popq	%r15
 4a514d2: 5d                           	popq	%rbp
 4a514d3: c3                           	retq
 4a514d4: e8 f7 15 87 02               	callq	0x72c2ad0 <plt___stack_chk_fail>
 4a514d9: 0f 0b                        	ud2
 4a514db: 90                           	nop
 4a514dc: 90                           	nop
 4a514dd: 90                           	nop
 4a514de: 90                           	nop
 4a514df: 90                           	nop
 4a514e0: 55                           	pushq	%rbp
 4a514e1: 48 89 e5                     	movq	%rsp, %rbp
 4a514e4: 41 57                        	pushq	%r15
 4a514e6: 41 56                        	pushq	%r14
 4a514e8: 41 55                        	pushq	%r13
 4a514ea: 41 54                        	pushq	%r12
 4a514ec: 53                           	pushq	%rbx
 4a514ed: 50                           	pushq	%rax
 4a514ee: 48 8d 05 33 c8 bf 03         	leaq	0x3bfc833(%rip), %rax   # 0x864dd28 <plt_sceZlibGetResult+0x1387598>
 4a514f5: 49 89 fe                     	movq	%rdi, %r14
 4a514f8: 48 89 07                     	movq	%rax, (%rdi)
 4a514fb: 48 8b 57 50                  	movq	0x50(%rdi), %rdx
 4a514ff: 8b 4f 0c                     	movl	0xc(%rdi), %ecx
