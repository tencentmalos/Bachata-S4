
build/validation/tetris-signin-20260921/eos-analysis.elf:	file format elf64-x86-64

Disassembly of section .text:

0000000000771b10 <urhplkgDIAU>:
  830ca0:      	pushq	%rbp
  830ca1:      	movq	%rsp, %rbp
  830ca4:      	pushq	%r15
  830ca6:      	pushq	%r14
  830ca8:      	pushq	%r12
  830caa:      	pushq	%rbx
  830cab:      	subq	$0x30, %rsp
  830caf:      	movq	0xe5d3fa(%rip), %r15    # 0x168e0b0 <plt_free+0x677a81>
  830cb6:      	movq	%rdi, %r14
  830cb9:      	movl	$0x1, %esi
  830cbe:      	xorl	%edx, %edx
  830cc0:      	movq	(%r15), %rax
  830cc3:      	movq	%rax, -0x28(%rbp)
  830cc7:      	movq	$-0x1, (%rdi)
  830cce:      	movl	$0x2, %edi
  830cd3:      	callq	0xcd8 <plt_socket>
  830cd8:      	testl	%eax, %eax
  830cda:      	js	0x830dc1 <urhplkgDIAU+0xbf2b1>
  830ce0:      	leaq	-0x40(%rbp), %rsi
  830ce4:      	movl	%eax, %edi
  830ce6:      	movl	$0x10, %edx
  830ceb:      	vxorps	%xmm0, %xmm0, %xmm0
  830cef:      	vmovups	%xmm0, -0x40(%rbp)
  830cf4:      	movb	$0x2, -0x3f(%rbp)
  830cf8:      	movl	$0x100007f, -0x3c(%rbp) # imm = 0x100007F
  830cff:      	movl	%eax, %ebx
  830d01:      	movw	$0x0, -0x3e(%rbp)
  830d07:      	callq	0xc98 <plt_bind>
  830d0c:      	testl	%eax, %eax
  830d0e:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d14:      	movl	%ebx, %edi
  830d16:      	movl	$0x1, %esi
  830d1b:      	callq	0xc08 <plt_listen>
  830d20:      	testl	%eax, %eax
  830d22:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d28:      	movl	$0x2, %edi
  830d2d:      	movl	$0x1, %esi
  830d32:      	xorl	%edx, %edx
  830d34:      	callq	0xcd8 <plt_socket>
  830d39:      	testl	%eax, %eax
  830d3b:      	movl	%eax, 0x4(%r14)
  830d3f:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d41:      	leaq	-0x40(%rbp), %rsi
  830d45:      	leaq	-0x44(%rbp), %rdx
  830d49:      	movl	%ebx, %edi
  830d4b:      	movl	$0x10, -0x44(%rbp)
  830d52:      	callq	0xc28 <plt_getsockname>
  830d57:      	testl	%eax, %eax
  830d59:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d5b:      	movl	0x4(%r14), %edi
  830d5f:      	leaq	-0x40(%rbp), %rsi
  830d63:      	movl	$0x10, %edx
  830d68:      	callq	0xce8 <plt_connect>
  830d6d:      	testl	%eax, %eax
  830d6f:      	js	0x830dba <urhplkgDIAU+0xbf2aa>
  830d71:      	leaq	-0x40(%rbp), %rsi
  830d75:      	leaq	-0x44(%rbp), %rdx
  830d79:      	movl	%ebx, %edi
  830d7b:      	callq	0xd08 <plt_accept>
  830d80:      	testl	%eax, %eax
  830d82:      	movl	%eax, (%r14)
  830d85:      	setns	%r12b
  830d89:      	js	0x830e2a <urhplkgDIAU+0xbf31a>
  830d8f:      	leaq	-0x48(%rbp), %rcx
  830d93:      	movl	%eax, %edi
  830d95:      	movl	$0x6, %esi
  830d9a:      	movl	$0x1, %edx
  830d9f:      	movl	$0x4, %r8d
  830da5:      	movl	$0x1, -0x48(%rbp)
  830dac:      	callq	0xc38 <plt_sceNetSetsockopt>
  830db1:      	testl	%eax, %eax
  830db3:      	je	0x830e07 <urhplkgDIAU+0xbf2f7>
  830db5:      	xorl	%r12d, %r12d
  830db8:      	jmp	0x830e2a <urhplkgDIAU+0xbf31a>
  830dba:      	movl	%ebx, %edi
  830dbc:      	callq	0xb48 <plt_sceNetSocketClose>
  830dc1:      	callq	0xb78 <plt_sceNetErrnoLoc>
  830dc6:      	movl	(%rax), %ebx
  830dc8:      	callq	0xbc8 <plt___error>
  830dcd:      	movl	%ebx, (%rax)
  830dcf:      	movl	(%r14), %edi
  830dd2:      	cmpl	$-0x1, %edi
  830dd5:      	je	0x830ddc <urhplkgDIAU+0xbf2cc>
  830dd7:      	callq	0xb48 <plt_sceNetSocketClose>
  830ddc:      	movl	0x4(%r14), %edi
  830de0:      	movl	$0xffffffff, %ebx       # imm = 0xFFFFFFFF
  830de5:      	cmpl	$-0x1, %edi
