
build/validation/tetris-signin-20260921/eos-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

00000000009bc3a0 <jn3f8YwVAoo>:
  a666c0:      	cmpl	%ecx, -0x76c3dbb4(%rbx)
  a666c6:      	andb	$0x1c, %al
  a666c9:      	movl	%eax, 0x19(%rsp)
  a666cd:      	movq	0x10(%rsp), %rax
  a666d2:      	movq	%rax, 0x20(%rsp)
  a666d7:      	movq	0x8(%rsp), %rax
  a666dc:      	movq	%rax, 0x28(%rsp)
  a666e1:      	cmpb	$0x0, 0x11(%rbx)
  a666e5:      	jne	0xa667d5 <jn3f8YwVAoo+0xaa435>
  a666eb:      	leaq	0x8(%rbx), %r13
  a666ef:      	movb	$0x1, %al
  a666f1:      	leaq	0x20(%rsp), %r14
  a666f6:      	nopw	%cs:(%rax,%rax)
  a66700:      	movq	%r13, %rdi
  a66703:      	movq	%rbx, %rsi
  a66706:      	testb	%al, %al
  a66708:      	je	0xa66720 <jn3f8YwVAoo+0xaa380>
  a6670a:      	movq	%r14, %rdx
  a6670d:      	callq	0x908 <plt_pthread_cond_timedwait>
  a66712:      	jmp	0xa66725 <jn3f8YwVAoo+0xaa385>
  a66714:      	nopw	%cs:(%rax,%rax)
  a66720:      	callq	0x988 <plt_pthread_cond_wait>
  a66725:      	movzbl	0x11(%rbx), %ecx
  a66729:      	testl	%eax, %eax
  a6672b:      	jne	0xa667a6 <jn3f8YwVAoo+0xaa406>
  a6672d:      	testb	%cl, %cl
  a6672f:      	jne	0xa667a6 <jn3f8YwVAoo+0xaa406>
  a66731:      	movzbl	0x18(%rsp), %eax
  a66736:      	jmp	0xa66700 <jn3f8YwVAoo+0xaa360>
  a66738:      	movb	%bpl, 0x18(%rsp)
  a6673d:      	movq	(%rsp), %rcx
  a66741:      	movl	(%rcx), %eax
  a66743:      	movl	0x3(%rcx), %ecx
  a66746:      	movl	%ecx, 0x1c(%rsp)
  a6674a:      	movl	%eax, 0x19(%rsp)
  a6674e:      	movq	%r15, 0x20(%rsp)
  a66753:      	movq	%r12, 0x28(%rsp)
  a66758:      	xorl	%eax, %eax
  a6675a:      	cmpb	$0x0, 0x11(%rbx)
  a6675e:      	jne	0xa667d1 <jn3f8YwVAoo+0xaa431>
  a66760:      	leaq	0x8(%rbx), %r12
  a66764:      	leaq	0x20(%rsp), %r14
  a66769:      	nopl	(%rax)
  a66770:      	movq	%r12, %rdi
  a66773:      	movq	%rbx, %rsi
  a66776:      	testb	%bpl, %bpl
  a66779:      	je	0xa66790 <jn3f8YwVAoo+0xaa3f0>
  a6677b:      	movq	%r14, %rdx
  a6677e:      	callq	0x908 <plt_pthread_cond_timedwait>
  a66783:      	testl	%eax, %eax
  a66785:      	je	0xa66799 <jn3f8YwVAoo+0xaa3f9>
  a66787:      	jmp	0xa667d1 <jn3f8YwVAoo+0xaa431>
  a66789:      	nopl	(%rax)
  a66790:      	callq	0x988 <plt_pthread_cond_wait>
  a66795:      	testl	%eax, %eax
  a66797:      	jne	0xa667d1 <jn3f8YwVAoo+0xaa431>
  a66799:      	cmpb	$0x0, 0x11(%rbx)
  a6679d:      	jne	0xa667d1 <jn3f8YwVAoo+0xaa431>
  a6679f:      	movzbl	0x18(%rsp), %ebp
  a667a4:      	jmp	0xa66770 <jn3f8YwVAoo+0xaa3d0>
  a667a6:      	cmpl	$0x3c, %eax
  a667a9:      	jne	0xa667d1 <jn3f8YwVAoo+0xaa431>
  a667ab:      	movb	%bpl, 0x18(%rsp)
  a667b0:      	movq	(%rsp), %rdx
  a667b4:      	movl	(%rdx), %eax
  a667b6:      	movl	%eax, 0x19(%rsp)
  a667ba:      	xorl	%eax, %eax
  a667bc:      	movl	0x3(%rdx), %edx
  a667bf:      	movl	%edx, 0x1c(%rsp)
