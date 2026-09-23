
build/validation/monster-hunter-network-gesture-20260922/mhr-v2-jit.o:	file format elf64-littleaarch64

Disassembly of section .text:

0000000000000000 <probe>:
       0: 00000009     	udf	#0x9
       4: 00000000     	udf	#0x0
       8: 00000003     	udf	#0x3
       c: 00000028     	udf	#0x28
		...
      18: 34230180     	cbz	w0, 0x46048 <probe+0x46048>
      1c: 00000000     	udf	#0x0
      20: 00000098     	udf	#0x98
      24: 10ffffe0     	adr	x0, 0x20 <probe+0x20>
      28: f9000380     	str	x0, [x28]
      2c: f91f639f     	str	xzr, [x28, #0x3ec0]
      30: 910041f4     	add	x20, x15, #0x10
      34: f8bfc284     	ldapr	x4, [x20]
      38: d503201f     	nop
      3c: 52800014     	mov	w20, #0x0               // =0
      40: 91006495     	add	x21, x4, #0x19
      44: 38bfc2bb     	ldaprb	w27, [x21]
      48: 53081f60     	lsl	w0, w27, #24
      4c: 6b14601f     	cmp	w0, w20, lsl #24
      50: 5100037a     	sub	w26, w27, #0x0
      54: 54000040     	b.eq	0x5c <probe+0x5c>
      58: 1400014b     	b	0x584 <probe+0x584>
      5c: 14000026     	b	0xf4 <probe+0xf4>
      60: 14000002     	b	0x68 <probe+0x68>
      64: d61f0000     	br	x0
      68: 58000240     	ldr	x0, 0xb0 <probe+0xb0>
      6c: d63f0000     	blr	x0
		...
      78: 042f5897     	addsvl	x23, x15, #0x4
      7c: 00000000     	udf	#0x0
      80: fffffff8     	<unknown>
      84: ffffffff     	<unknown>
      88: 14000002     	b	0x90 <probe+0x90>
      8c: d61f0000     	br	x0
      90: 58000100     	ldr	x0, 0xb0 <probe+0xb0>
      94: d63f0000     	blr	x0
		...
      a0: 042f59f1     	addsvl	x17, x15, #0xf
      a4: 00000000     	udf	#0x0
      a8: ffffffd4     	<unknown>
      ac: ffffffff     	<unknown>
      b0: 868eb0f8     	<unknown>
      b4: 00000071     	udf	#0x71
      b8: 000000d0     	udf	#0xd0
      bc: 00000000     	udf	#0x0
      c0: 042f5889     	addsvl	x9, x15, #0x4
      c4: 00000000     	udf	#0x0
      c8: 0000000e     	udf	#0xe
      cc: 00000000     	udf	#0x0
      d0: 00000004     	udf	#0x4
      d4: 00000028     	udf	#0x28
		...
      e0: 03800180     	<unknown>
      e4: 00003532     	udf	#0x3532
		...
      f0: 00000050     	udf	#0x50
      f4: 10ffffe0     	adr	x0, 0xf0 <probe+0xf0>
      f8: f9000380     	str	x0, [x28]
      fc: f91f639f     	str	xzr, [x28, #0x3ec0]
