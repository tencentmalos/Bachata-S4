
build/validation/psvr-followup-20260920/tetris-main.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000000020 <module_init>:
  4f80c0: 55                           	pushq	%rbp
  4f80c1: 48 89 e5                     	movq	%rsp, %rbp
  4f80c4: 41 57                        	pushq	%r15
  4f80c6: 41 56                        	pushq	%r14
  4f80c8: 41 55                        	pushq	%r13
  4f80ca: 41 54                        	pushq	%r12
  4f80cc: 53                           	pushq	%rbx
  4f80cd: 48 83 ec 58                  	subq	$0x58, %rsp
  4f80d1: 4c 8b 2d 70 af 07 06         	movq	0x607af70(%rip), %r13   # 0x6573048 <plt_log10+0x1ba2af8>
  4f80d8: 48 8d 55 b0                  	leaq	-0x50(%rbp), %rdx
  4f80dc: 48 8d 4d a8                  	leaq	-0x58(%rbp), %rcx
  4f80e0: 49 89 fc                     	movq	%rdi, %r12
  4f80e3: 49 8b 45 00                  	movq	(%r13), %rax
  4f80e7: 48 89 45 d0                  	movq	%rax, -0x30(%rbp)
  4f80eb: 48 89 75 b8                  	movq	%rsi, -0x48(%rbp)
  4f80ef: 48 8d 75 b8                  	leaq	-0x48(%rbp), %rsi
  4f80f3: 48 c7 45 b0 00 00 00 00      	movq	$0x0, -0x50(%rbp)
  4f80fb: 48 c7 45 a8 00 00 00 00      	movq	$0x0, -0x58(%rbp)
  4f8103: e8 a8 fb ff ff               	callq	0x4f7cb0 <module_init+0x4f7c90>
  4f8108: 48 83 7d b0 00               	cmpq	$0x0, -0x50(%rbp)
  4f810d: 48 89 45 a0                  	movq	%rax, -0x60(%rbp)
  4f8111: 0f 84 c1 01 00 00            	je	0x4f82d8 <module_init+0x4f82b8>
  4f8117: 48 89 c3                     	movq	%rax, %rbx
  4f811a: 41 8b 84 24 c0 00 00 00      	movl	0xc0(%r12), %eax
  4f8122: 41 3b 84 24 ec 00 00 00      	cmpl	0xec(%r12), %eax
  4f812a: 4d 8d b4 24 b8 00 00 00      	leaq	0xb8(%r12), %r14
  4f8132: 74 66                        	je	0x4f819a <module_init+0x4f817a>
  4f8134: 49 89 df                     	movq	%rbx, %r15
  4f8137: 89 df                        	movl	%ebx, %edi
  4f8139: 49 c1 ef 20                  	shrq	$0x20, %r15
  4f813d: e8 be d6 cf 00               	callq	0x11f5800 <module_init+0x11f57e0>
  4f8142: 49 8b 94 24 f8 00 00 00      	movq	0xf8(%r12), %rdx
  4f814a: 49 63 b4 24 00 01 00 00      	movslq	0x100(%r12), %rsi
  4f8152: 44 01 f8                     	addl	%r15d, %eax
  4f8155: 49 8d 8c 24 f0 00 00 00      	leaq	0xf0(%r12), %rcx
  4f815d: 48 98                        	cltq
  4f815f: 48 85 d2                     	testq	%rdx, %rdx
  4f8162: 48 0f 45 ca                  	cmovneq	%rdx, %rcx
  4f8166: 48 ff ce                     	decq	%rsi
  4f8169: 48 21 f0                     	andq	%rsi, %rax
  4f816c: 8b 0c 81                     	movl	(%rcx,%rax,4), %ecx
  4f816f: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f8172: 74 26                        	je	0x4f819a <module_init+0x4f817a>
  4f8174: 49 8b 06                     	movq	(%r14), %rax
  4f8177: 66 0f 1f 84 00 00 00 00 00   	nopw	(%rax,%rax)
  4f8180: 48 63 c9                     	movslq	%ecx, %rcx
  4f8183: 48 c1 e1 05                  	shlq	$0x5, %rcx
  4f8187: 48 39 1c 08                  	cmpq	%rbx, (%rax,%rcx)
  4f818b: 0f 84 6a 01 00 00            	je	0x4f82fb <module_init+0x4f82db>
  4f8191: 8b 4c 08 18                  	movl	0x18(%rax,%rcx), %ecx
  4f8195: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f8198: 75 e6                        	jne	0x4f8180 <module_init+0x4f8160>
  4f819a: 48 8d 7d b0                  	leaq	-0x50(%rbp), %rdi
  4f819e: 48 8d 75 c0                  	leaq	-0x40(%rbp), %rsi
  4f81a2: 48 c7 45 c0 00 00 00 00      	movq	$0x0, -0x40(%rbp)
  4f81aa: e8 e1 c6 ff ff               	callq	0x4f4890 <module_init+0x4f4870>
  4f81af: 84 c0                        	testb	%al, %al
  4f81b1: 0f 84 21 01 00 00            	je	0x4f82d8 <module_init+0x4f82b8>
  4f81b7: 41 8b 44 24 70               	movl	0x70(%r12), %eax
  4f81bc: 41 3b 84 24 9c 00 00 00      	cmpl	0x9c(%r12), %eax
  4f81c4: 74 64                        	je	0x4f822a <module_init+0x4f820a>
  4f81c6: 48 8b 5d b0                  	movq	-0x50(%rbp), %rbx
  4f81ca: 49 89 df                     	movq	%rbx, %r15
  4f81cd: 89 df                        	movl	%ebx, %edi
  4f81cf: 49 c1 ef 20                  	shrq	$0x20, %r15
  4f81d3: e8 28 d6 cf 00               	callq	0x11f5800 <module_init+0x11f57e0>
  4f81d8: 49 8b 94 24 a8 00 00 00      	movq	0xa8(%r12), %rdx
  4f81e0: 49 63 b4 24 b0 00 00 00      	movslq	0xb0(%r12), %rsi
  4f81e8: 44 01 f8                     	addl	%r15d, %eax
  4f81eb: 49 8d 8c 24 a0 00 00 00      	leaq	0xa0(%r12), %rcx
  4f81f3: 48 98                        	cltq
  4f81f5: 48 85 d2                     	testq	%rdx, %rdx
  4f81f8: 48 0f 45 ca                  	cmovneq	%rdx, %rcx
  4f81fc: 48 ff ce                     	decq	%rsi
  4f81ff: 48 21 f0                     	andq	%rsi, %rax
  4f8202: 8b 0c 81                     	movl	(%rcx,%rax,4), %ecx
  4f8205: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f8208: 74 20                        	je	0x4f822a <module_init+0x4f820a>
  4f820a: 49 8b 44 24 68               	movq	0x68(%r12), %rax
  4f820f: 90                           	nop
  4f8210: 48 63 c9                     	movslq	%ecx, %rcx
  4f8213: 48 8d 0c 49                  	leaq	(%rcx,%rcx,2), %rcx
  4f8217: 48 39 1c c8                  	cmpq	%rbx, (%rax,%rcx,8)
  4f821b: 0f 84 f2 00 00 00            	je	0x4f8313 <module_init+0x4f82f3>
  4f8221: 8b 4c c8 10                  	movl	0x10(%rax,%rcx,8), %ecx
  4f8225: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f8228: 75 e6                        	jne	0x4f8210 <module_init+0x4f81f0>
  4f822a: 4c 8d 7d c0                  	leaq	-0x40(%rbp), %r15
  4f822e: 48 8d 75 b0                  	leaq	-0x50(%rbp), %rsi
  4f8232: 4c 89 ff                     	movq	%r15, %rdi
  4f8235: e8 b6 3d d0 00               	callq	0x11fbff0 <module_init+0x11fbfd0>
  4f823a: 49 8d b4 24 58 01 00 00      	leaq	0x158(%r12), %rsi
  4f8242: 4c 89 ff                     	movq	%r15, %rdi
  4f8245: e8 76 ee ff ff               	callq	0x4f70c0 <module_init+0x4f70a0>
  4f824a: 4c 89 ff                     	movq	%r15, %rdi
  4f824d: 48 89 c3                     	movq	%rax, %rbx
  4f8250: e8 eb 6c b1 ff               	callq	0xef40 <module_init+0xef20>
  4f8255: 48 85 db                     	testq	%rbx, %rbx
  4f8258: 0f 84 7a 00 00 00            	je	0x4f82d8 <module_init+0x4f82b8>
  4f825e: 41 8b 44 24 70               	movl	0x70(%r12), %eax
  4f8263: 41 3b 84 24 9c 00 00 00      	cmpl	0x9c(%r12), %eax
  4f826b: 74 6b                        	je	0x4f82d8 <module_init+0x4f82b8>
  4f826d: 48 8b 5d b0                  	movq	-0x50(%rbp), %rbx
  4f8271: 49 89 df                     	movq	%rbx, %r15
  4f8274: 89 df                        	movl	%ebx, %edi
  4f8276: 49 c1 ef 20                  	shrq	$0x20, %r15
  4f827a: e8 81 d5 cf 00               	callq	0x11f5800 <module_init+0x11f57e0>
  4f827f: 49 8b 94 24 a8 00 00 00      	movq	0xa8(%r12), %rdx
  4f8287: 49 63 b4 24 b0 00 00 00      	movslq	0xb0(%r12), %rsi
  4f828f: 44 01 f8                     	addl	%r15d, %eax
  4f8292: 49 8d 8c 24 a0 00 00 00      	leaq	0xa0(%r12), %rcx
  4f829a: 48 98                        	cltq
  4f829c: 48 85 d2                     	testq	%rdx, %rdx
  4f829f: 48 0f 45 ca                  	cmovneq	%rdx, %rcx
  4f82a3: 48 ff ce                     	decq	%rsi
  4f82a6: 48 21 f0                     	andq	%rsi, %rax
  4f82a9: 8b 0c 81                     	movl	(%rcx,%rax,4), %ecx
  4f82ac: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f82af: 74 27                        	je	0x4f82d8 <module_init+0x4f82b8>
  4f82b1: 49 8b 44 24 68               	movq	0x68(%r12), %rax
  4f82b6: 45 31 ff                     	xorl	%r15d, %r15d
  4f82b9: 0f 1f 80 00 00 00 00         	nopl	(%rax)
  4f82c0: 48 63 c9                     	movslq	%ecx, %rcx
  4f82c3: 48 8d 0c 49                  	leaq	(%rcx,%rcx,2), %rcx
  4f82c7: 48 39 1c c8                  	cmpq	%rbx, (%rax,%rcx,8)
  4f82cb: 74 46                        	je	0x4f8313 <module_init+0x4f82f3>
  4f82cd: 8b 4c c8 10                  	movl	0x10(%rax,%rcx,8), %ecx
  4f82d1: 83 f9 ff                     	cmpl	$-0x1, %ecx
  4f82d4: 75 ea                        	jne	0x4f82c0 <module_init+0x4f82a0>
  4f82d6: eb 03                        	jmp	0x4f82db <module_init+0x4f82bb>
  4f82d8: 45 31 ff                     	xorl	%r15d, %r15d
  4f82db: 49 8b 45 00                  	movq	(%r13), %rax
  4f82df: 48 3b 45 d0                  	cmpq	-0x30(%rbp), %rax
  4f82e3: 0f 85 97 01 00 00            	jne	0x4f8480 <module_init+0x4f8460>
  4f82e9: 4c 89 f8                     	movq	%r15, %rax
  4f82ec: 48 83 c4 58                  	addq	$0x58, %rsp
  4f82f0: 5b                           	popq	%rbx
  4f82f1: 41 5c                        	popq	%r12
  4f82f3: 41 5d                        	popq	%r13
  4f82f5: 41 5e                        	popq	%r14
  4f82f7: 41 5f                        	popq	%r15
  4f82f9: 5d                           	popq	%rbp
  4f82fa: c3                           	retq
  4f82fb: 48 8b 5c 08 10               	movq	0x10(%rax,%rcx), %rbx
  4f8300: 4c 8b 7c 08 08               	movq	0x8(%rax,%rcx), %r15
  4f8305: 48 85 db                     	testq	%rbx, %rbx
  4f8308: 74 d1                        	je	0x4f82db <module_init+0x4f82bb>
  4f830a: f0                           	lock
  4f830b: ff 43 08                     	incl	0x8(%rbx)
  4f830e: e9 43 01 00 00               	jmp	0x4f8456 <module_init+0x4f8436>
  4f8313: 48 8d 44 c8 08               	leaq	0x8(%rax,%rcx,8), %rax
  4f8318: 48 8b 55 a8                  	movq	-0x58(%rbp), %rdx
  4f831c: 48 8d 5d 90                  	leaq	-0x70(%rbp), %rbx
  4f8320: 48 89 df                     	movq	%rbx, %rdi
  4f8323: 48 8b 30                     	movq	(%rax), %rsi
  4f8326: 48 8b 06                     	movq	(%rsi), %rax
  4f8329: ff 50 10                     	callq	*0x10(%rax)
  4f832c: 48 83 7d 90 00               	cmpq	$0x0, -0x70(%rbp)
  4f8331: 74 43                        	je	0x4f8376 <module_init+0x4f8356>
  4f8333: 48 8d 45 a0                  	leaq	-0x60(%rbp), %rax
  4f8337: 48 8d 75 c0                  	leaq	-0x40(%rbp), %rsi
  4f833b: 4c 89 f7                     	movq	%r14, %rdi
  4f833e: 31 d2                        	xorl	%edx, %edx
