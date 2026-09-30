__int64 __fastcall fn_103b390(__int64 _RDI)
{
  __int64 v531; // rax
  int v532; // ecx
  int v533; // edx
  __int64 i; // rsi
  int v540; // r11d
  __int64 v542; // r14
  _DWORD *v543; // r12
  bool v544; // zf
  _DWORD v558[4]; // [rsp+20h] [rbp-70h] BYREF
  int v562; // [rsp+3Ch] [rbp-54h]
  int v566; // [rsp+4Ch] [rbp-44h]
  __int64 v568; // [rsp+68h] [rbp-28h]

  _RBX = _RDI;
  v568 = 0x6365786562696C2FLL;
  __asm
  {
    vmovsd  xmm9, qword ptr [rdi+0E0h]
    vmovss  xmm0, dword ptr [rdi+0E8h]
    vmovsd  xmm11, qword ptr [rdi+90h]
    vmovsd  xmm2, qword ptr [rdi+88h]
    vbroadcastss xmm7, dword ptr [rdi+0ECh]
    vmovsd  xmm4, qword ptr [rdi+98h]
    vmovsd  xmm5, qword ptr [rdi+0A0h]
    vshufps xmm3, xmm9, xmm9, 0E1h
    vinsertps xmm14, xmm11, xmm0, 10h
    vshufps xmm1, xmm11, xmm11, 0E1h
    vinsertps xmm8, xmm9, xmm0, 10h
    vblendps xmm6, xmm9, xmm0, 1
    vinsertps xmm13, xmm3, xmm2, 1Ch
    vmulps  xmm12, xmm7, xmm1
    vmulps  xmm1, xmm7, xmm2
    vmulps  xmm10, xmm7, xmm4
    vmulps  xmm13, xmm13, xmm14
    vmovshdup xmm14, xmm11
    vmulps  xmm14, xmm14, xmm9
    vaddps  xmm1, xmm14, xmm1
    vmovsldup xmm14, xmm11
    vinsertps xmm14, xmm14, xmm2, 4Ch ; 'L'
    vaddps  xmm1, xmm13, xmm1
    vblendps xmm13, xmm3, xmm0, 1
    vmulps  xmm14, xmm13, xmm14
    vsubps  xmm1, xmm1, xmm14
    vinsertps xmm14, xmm2, xmm11, 50h ; 'P'
    vmulps  xmm14, xmm14, xmm8
    vaddps  xmm15, xmm12, xmm14
    vsubps  xmm12, xmm12, xmm14
    vmovshdup xmm14, xmm2
    vinsertps xmm2, xmm11, xmm2, 1Ch
    vmulps  xmm14, xmm14, xmm3
    vmulps  xmm2, xmm2, xmm6
    vaddps  xmm15, xmm15, xmm14
    vsubps  xmm12, xmm12, xmm14
    vinsertps xmm14, xmm5, xmm0, 10h
    vblendps xmm12, xmm12, xmm15, 2
    vbroadcastss xmm15, cs:dword_18248B4
    vsubps  xmm2, xmm12, xmm2
    vinsertps xmm12, xmm3, xmm4, 1Ch
    vmulps  xmm12, xmm12, xmm14
    vmovshdup xmm14, xmm5
    vmulps  xmm9, xmm14, xmm9
    vshufps xmm11, xmm2, xmm2, 0E1h
    vaddps  xmm9, xmm9, xmm10
    vmovsldup xmm10, xmm5
    vxorps  xmm11, xmm11, xmm15
    vinsertps xmm10, xmm10, xmm4, 4Ch ; 'L'
    vmulps  xmm10, xmm13, xmm10
    vaddps  xmm9, xmm9, xmm12
    vshufps xmm12, xmm5, xmm5, 0E1h
    vmulps  xmm7, xmm12, xmm7
    vinsertps xmm12, xmm4, xmm5, 50h ; 'P'
    vmulps  xmm8, xmm12, xmm8
    vsubps  xmm9, xmm9, xmm10
    vaddps  xmm12, xmm8, xmm7
    vsubps  xmm7, xmm7, xmm8
    vmovshdup xmm8, xmm4
    vinsertps xmm4, xmm5, xmm4, 1Ch
    vmulps  xmm3, xmm8, xmm3
    vmulps  xmm4, xmm4, xmm6
    vinsertf128 ymm10, ymm1, xmm9, 1
    vaddps  xmm8, xmm12, xmm3
    vsubps  xmm3, xmm7, xmm3
    vblendps xmm3, xmm3, xmm8, 2
    vsubps  xmm3, xmm3, xmm4
    vxorps  xmm4, xmm15, xmm3
    vmulps  xmm6, xmm2, xmm3
    vshufps xmm5, xmm4, xmm4, 0E1h
    vinsertf128 ymm5, ymm11, xmm5, 1
    vmovshdup xmm7, xmm6
    vunpcklpd ymm5, ymm10, ymm5
    vmovups ymmword ptr [rdi+30h], ymm5
    vmulps  xmm5, xmm9, xmm1
    vhaddps xmm5, xmm5, xmm5
    vaddss  xmm5, xmm5, xmm7
    vaddss  xmm5, xmm5, xmm6
    vxorps  xmm6, xmm6, xmm6
    vucomiss xmm6, xmm5
  }
  __asm
  {
    vxorps  xmm9, xmm9, xmm15
    vbroadcastss xmm5, cs:dword_18248A4
    vsubps  xmm4, xmm9, xmm1
    vaddps  xmm3, xmm3, xmm2
    vmovss  xmm13, cs:dword_18248A0
  }
  __asm
  {
    vmulps  xmm4, xmm4, xmm5
    vmulps  xmm3, xmm3, xmm5
    vaddps  xmm1, xmm1, xmm4
    vsubps  xmm2, xmm3, xmm2
    vmulps  xmm4, xmm1, xmm1
    vmulps  xmm3, xmm2, xmm2
    vhaddps xmm4, xmm4, xmm4
    vmovshdup xmm5, xmm3
    vaddss  xmm4, xmm5, xmm4
    vaddss  xmm3, xmm3, xmm4
    vsqrtss xmm3, xmm3, xmm3
    vmovsldup xmm3, xmm3
    vdivps  xmm1, xmm1, xmm3
    vdivps  xmm2, xmm2, xmm3
    vshufps xmm1, xmm1, xmm2, 14h
    vmovups xmmword ptr [rbx+50h], xmm1
    vmovss  xmm1, dword ptr [rbx+0ECh]
    vmovss  xmm4, dword ptr [rbx+0A8h]
    vmovss  xmm2, dword ptr [rbx+0E0h]
    vmovss  xmm5, dword ptr [rbx+0B4h]
    vmovss  xmm7, dword ptr [rbx+0B0h]
    vmulss  xmm3, xmm1, xmm4
    vmulss  xmm6, xmm2, xmm5
    vaddss  xmm6, xmm3, xmm6
    vmovss  xmm3, dword ptr [rbx+0E4h]
    vmulss  xmm8, xmm3, xmm7
    vmulss  xmm10, xmm5, xmm3
    vaddss  xmm6, xmm8, xmm6
    vmovss  xmm8, dword ptr [rbx+0ACh]
    vmulss  xmm9, xmm8, xmm0
    vsubss  xmm6, xmm6, xmm9
    vmulss  xmm9, xmm8, xmm1
    vaddss  xmm9, xmm10, xmm9
    vmulss  xmm10, xmm4, xmm0
    vaddss  xmm9, xmm10, xmm9
    vmulss  xmm10, xmm2, xmm7
    vsubss  xmm9, xmm9, xmm10
    vmulss  xmm10, xmm5, xmm0
    vmulss  xmm5, xmm1, xmm5
    vinsertps xmm6, xmm6, xmm9, 10h
    vmulss  xmm9, xmm1, xmm7
    vaddss  xmm9, xmm9, xmm10
    vmulss  xmm10, xmm8, xmm2
    vaddss  xmm9, xmm9, xmm10
    vmulss  xmm10, xmm4, xmm3
    vmulss  xmm4, xmm4, xmm2
    vsubss  xmm4, xmm5, xmm4
    vmulss  xmm5, xmm8, xmm3
    vsubss  xmm9, xmm9, xmm10
    vsubss  xmm4, xmm4, xmm5
    vmulss  xmm5, xmm7, xmm0
    vsubss  xmm4, xmm4, xmm5
    vxorps  xmm5, xmm9, xmm15
    vxorps  xmm4, xmm15, xmm4
    vunpcklps xmm4, xmm5, xmm4
    vaddss  xmm5, xmm0, xmm0
    vmovlhps xmm4, xmm6, xmm4
    vmovups xmmword ptr [rbx+60h], xmm4
    vmovss  xmm4, dword ptr [rbx+0F0h]
    vaddss  xmm6, xmm4, dword ptr [rbx+70h]
    vmovss  xmm4, dword ptr [rbx+0F4h]
    vaddss  xmm7, xmm4, dword ptr [rbx+74h]
    vaddss  xmm4, xmm3, xmm3
    vmulss  xmm9, xmm0, xmm5
    vmulss  xmm11, xmm1, xmm5
    vmulss  xmm15, xmm2, xmm5
    vmulss  xmm8, xmm3, xmm4
    vmulss  xmm10, xmm2, xmm4
    vmulss  xmm14, xmm4, xmm1
    vaddss  xmm12, xmm8, xmm9
    vaddss  xmm4, xmm15, xmm14
    vsubss  xmm12, xmm13, xmm12
    vsubss  xmm13, xmm10, xmm11
    vaddss  xmm10, xmm10, xmm11
    vaddss  xmm11, xmm2, xmm2
    vmulss  xmm12, xmm12, xmm6
    vmulss  xmm13, xmm13, xmm7
    vmulss  xmm10, xmm10, xmm6
    vaddss  xmm12, xmm13, xmm12
    vmovss  xmm13, dword ptr [rbx+0F8h]
    vaddss  xmm13, xmm13, dword ptr [rbx+78h]
    vmulss  xmm4, xmm13, xmm4
    vaddss  xmm4, xmm12, xmm4
    vmulss  xmm12, xmm11, xmm2
    vmulss  xmm11, xmm11, xmm1
    vaddss  xmm9, xmm12, xmm9
    vmovaps [rbp+var_90], xmm4
    vmovss  xmm4, cs:dword_18248A0
    vsubss  xmm9, xmm4, xmm9
    vmulss  xmm9, xmm9, xmm7
    vaddss  xmm9, xmm10, xmm9
    vmulss  xmm10, xmm3, xmm5
    vsubss  xmm5, xmm10, xmm11
    vmulss  xmm5, xmm13, xmm5
    vaddss  xmm5, xmm9, xmm5
    vsubss  xmm9, xmm15, xmm14
    vmulss  xmm6, xmm9, xmm6
    vaddss  xmm9, xmm10, xmm11
    vmovss  xmm10, dword ptr [rbx+94h]
    vmulss  xmm7, xmm9, xmm7
    vaddss  xmm6, xmm6, xmm7
    vaddss  xmm7, xmm12, xmm8
    vmulss  xmm11, xmm10, xmm2
    vmovss  xmm8, dword ptr [rbx+8Ch]
    vsubss  xmm7, xmm4, xmm7
    vmulss  xmm7, xmm13, xmm7
    vmulss  xmm13, xmm10, xmm3
    vaddss  xmm6, xmm7, xmm6
    vmovss  xmm7, dword ptr [rbx+88h]
    vmulss  xmm9, xmm1, xmm7
    vaddss  xmm9, xmm9, xmm11
    vmovss  xmm11, dword ptr [rbx+90h]
    vmulss  xmm12, xmm11, xmm3
    vaddss  xmm9, xmm9, xmm12
    vmulss  xmm12, xmm8, xmm0
    vsubss  xmm9, xmm9, xmm12
    vmulss  xmm12, xmm8, xmm1
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm0, xmm7
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm11, xmm2
    vsubss  xmm12, xmm12, xmm13
    vmulss  xmm13, xmm10, xmm0
    vmulss  xmm0, xmm11, xmm0
    vinsertps xmm9, xmm9, xmm12, 10h
    vmulss  xmm12, xmm11, xmm1
    vmulss  xmm1, xmm10, xmm1
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm8, xmm2
    vmulss  xmm2, xmm2, xmm7
    vsubss  xmm1, xmm1, xmm2
    vmulss  xmm2, xmm8, xmm3
    vaddss  xmm12, xmm12, xmm13
    vmulss  xmm13, xmm3, xmm7
    vbroadcastss xmm3, cs:dword_18248B4
    vsubss  xmm1, xmm1, xmm2
    vsubss  xmm12, xmm12, xmm13
    vsubss  xmm0, xmm1, xmm0
    vxorps  xmm1, xmm9, xmm3
    vmulss  xmm9, xmm12, cs:dword_1829FC8
    vmovaps xmm13, xmm3
    vshufps xmm7, xmm1, xmm1, 0F5h
    vxorps  xmm2, xmm12, xmm3
    vaddss  xmm3, xmm1, xmm1
    vaddss  xmm8, xmm7, xmm7
    vmulss  xmm10, xmm1, xmm3
    vmulss  xmm3, xmm3, xmm0
    vmulss  xmm11, xmm8, xmm7
    vmulss  xmm12, xmm8, xmm1
    vmulss  xmm8, xmm8, xmm0
    vmulss  xmm2, xmm9, xmm2
    vmulss  xmm1, xmm9, xmm1
    vmulss  xmm0, xmm9, xmm0
    vmulss  xmm7, xmm9, xmm7
    vaddss  xmm9, xmm11, xmm2
    vaddss  xmm15, xmm12, xmm0
    vsubss  xmm14, xmm1, xmm8
    vsubss  xmm0, xmm12, xmm0
    vsubss  xmm9, xmm4, xmm9
    vmovss  dword ptr [rbp+var_70], xmm9
    vmovss  dword ptr [rbp+var_70+4], xmm15
    vmovss  dword ptr [rbp+var_70+8], xmm14
  }
  v558[3] = 0;
  __asm
  {
    vmovss  dword ptr [rbp+var_60], xmm0
    vaddss  xmm0, xmm10, xmm2
    vsubss  xmm0, xmm4, xmm0
    vmovss  dword ptr [rbp+var_60+4], xmm0
    vaddss  xmm0, xmm7, xmm3
    vmovss  dword ptr [rbp+var_60+8], xmm0
    vaddss  xmm0, xmm8, xmm1
  }
  v562 = 0;
  __asm
  {
    vpermilps xmm1, [rbp+var_90], 0
    vmovss  dword ptr [rbp+var_50], xmm0
    vsubss  xmm0, xmm7, xmm3
    vmovss  dword ptr [rbp+var_50+4], xmm0
    vaddss  xmm0, xmm10, xmm11
    vsubss  xmm0, xmm4, xmm0
    vmovss  dword ptr [rbp+var_50+8], xmm0
    vxorps  xmm0, xmm0, xmm0
    vmovups [rbp+var_50+0Ch], xmm0
  }
  v566 = _RT0;
  __asm { vxorps  xmm0, xmm13, xmm5 }
  __asm
  {
    vshufps xmm0, xmm0, xmm0, 0
    vmulps  xmm0, xmm0, [rbp+var_60]
    vmulps  xmm1, xmm1, [rbp+var_70]
    vsubps  xmm0, xmm0, xmm1
    vshufps xmm1, xmm6, xmm6, 0
    vmulps  xmm1, xmm1, [rbp+var_50]
    vsubps  xmm0, xmm0, xmm1
    vaddps  xmm0, xmm0, xmmword ptr [rbp-40h]
    vmovups xmmword ptr [rbp-40h], xmm0
  }
  fn_a50dc0(a1: v558, a2: v558);
  __asm
  {
    vmovss  xmm0, dword ptr [rbp+var_40+8]
    vxorps  xmm0, xmm0, cs:xmmword_18249F0
    vmovsd  xmm1, qword ptr [rbp+var_40]
  }
  __asm
  {
    vmovsd  qword ptr [rbx], xmm1
    vmovss  dword ptr [rbx+8], xmm0
    vmovss  xmm5, dword ptr [rbx+0E8h]
    vmovss  xmm4, dword ptr [rbx+0E4h]
    vmovss  xmm0, dword ptr [rbx+0F0h]
    vaddss  xmm7, xmm0, dword ptr [rbx+7Ch]
    vmovss  xmm0, dword ptr [rbx+0F4h]
    vaddss  xmm8, xmm0, dword ptr [rbx+80h]
    vmovss  xmm0, dword ptr [rbx+0F8h]
    vaddss  xmm2, xmm0, dword ptr [rbx+84h]
    vmovss  xmm3, dword ptr [rbx+0E0h]
    vmovss  xmm6, dword ptr [rbx+0ECh]
    vaddss  xmm0, xmm4, xmm4
    vaddss  xmm1, xmm5, xmm5
    vmulss  xmm9, xmm4, xmm0
    vmulss  xmm10, xmm5, xmm1
    vmulss  xmm11, xmm3, xmm0
    vmulss  xmm13, xmm0, xmm6
    vmulss  xmm14, xmm6, xmm1
    vmulss  xmm12, xmm3, xmm1
    vmovss  [rbp+var_74], xmm9
    vaddss  xmm0, xmm9, xmm10
    vmovss  xmm9, cs:dword_18248A0
    vsubss  xmm15, xmm11, xmm14
    vaddss  xmm11, xmm11, xmm14
    vmulss  xmm15, xmm8, xmm15
    vsubss  xmm0, xmm9, xmm0
    vmulss  xmm0, xmm7, xmm0
    vaddss  xmm0, xmm15, xmm0
    vaddss  xmm15, xmm12, xmm13
    vmulss  xmm15, xmm15, xmm2
    vaddss  xmm0, xmm15, xmm0
    vaddss  xmm15, xmm3, xmm3
    vmulss  xmm14, xmm15, xmm3
    vmovaps [rbp+var_90], xmm0
    vmulss  xmm0, xmm4, xmm1
    vmulss  xmm15, xmm15, xmm6
    vmulss  xmm1, xmm11, xmm7
    vaddss  xmm10, xmm14, xmm10
    vsubss  xmm10, xmm9, xmm10
    vmulss  xmm10, xmm8, xmm10
    vaddss  xmm1, xmm10, xmm1
    vsubss  xmm10, xmm0, xmm15
    vaddss  xmm0, xmm15, xmm0
    vmulss  xmm10, xmm10, xmm2
    vmulss  xmm0, xmm8, xmm0
    vaddss  xmm1, xmm10, xmm1
    vsubss  xmm10, xmm12, xmm13
    vmulss  xmm7, xmm10, xmm7
    vaddss  xmm0, xmm7, xmm0
    vaddss  xmm7, xmm14, [rbp+var_74]
    vmovaps xmm14, xmm9
    vsubss  xmm7, xmm9, xmm7
    vmovss  xmm9, dword ptr [rbx+0A4h]
    vmulss  xmm2, xmm2, xmm7
    vmovss  xmm7, dword ptr [rbx+9Ch]
    vaddss  xmm2, xmm2, xmm0
    vmovss  xmm0, dword ptr [rbx+98h]
    vmulss  xmm10, xmm9, xmm3
    vmulss  xmm12, xmm9, xmm4
    vmulss  xmm8, xmm6, xmm0
    vaddss  xmm8, xmm8, xmm10
    vmovss  xmm10, dword ptr [rbx+0A0h]
    vmulss  xmm11, xmm10, xmm4
    vaddss  xmm8, xmm8, xmm11
    vmulss  xmm11, xmm5, xmm7
    vsubss  xmm8, xmm8, xmm11
    vmulss  xmm11, xmm6, xmm7
    vaddss  xmm11, xmm12, xmm11
    vmulss  xmm12, xmm5, xmm0
    vaddss  xmm11, xmm12, xmm11
    vmulss  xmm12, xmm10, xmm3
    vsubss  xmm11, xmm11, xmm12
    vmulss  xmm12, xmm9, xmm5
    vinsertps xmm8, xmm8, xmm11, 10h
    vmulss  xmm11, xmm10, xmm6
    vmulss  xmm6, xmm9, xmm6
    vaddss  xmm11, xmm12, xmm11
    vmulss  xmm12, xmm3, xmm7
    vaddss  xmm11, xmm11, xmm12
    vmulss  xmm12, xmm4, xmm0
    vmulss  xmm0, xmm3, xmm0
    vmulss  xmm3, xmm4, xmm7
    vsubss  xmm0, xmm6, xmm0
    vsubss  xmm11, xmm11, xmm12
    vsubss  xmm0, xmm0, xmm3
    vmulss  xmm3, xmm10, xmm5
    vbroadcastss xmm5, cs:dword_18248B4
    vsubss  xmm0, xmm0, xmm3
    vxorps  xmm3, xmm8, xmm5
    vmulss  xmm8, xmm11, cs:dword_1829FC8
    vxorps  xmm4, xmm11, xmm5
    vmovaps xmm13, xmm5
    vshufps xmm6, xmm3, xmm3, 0F5h
    vaddss  xmm5, xmm3, xmm3
    vaddss  xmm7, xmm6, xmm6
    vmulss  xmm9, xmm3, xmm5
    vmulss  xmm5, xmm5, xmm0
    vmulss  xmm10, xmm6, xmm7
    vmulss  xmm11, xmm3, xmm7
    vmulss  xmm7, xmm7, xmm0
    vmulss  xmm4, xmm8, xmm4
    vmulss  xmm3, xmm8, xmm3
    vmulss  xmm0, xmm8, xmm0
    vmulss  xmm6, xmm8, xmm6
    vaddss  xmm8, xmm10, xmm4
    vaddss  xmm15, xmm11, xmm0
    vsubss  xmm12, xmm3, xmm7
    vsubss  xmm0, xmm11, xmm0
    vsubss  xmm8, xmm14, xmm8
    vmovss  dword ptr [rbp+var_70], xmm8
    vmovss  dword ptr [rbp+var_70+4], xmm15
    vmovss  dword ptr [rbp+var_70+8], xmm12
  }
  v558[3] = 0;
  __asm
  {
    vmovss  dword ptr [rbp+var_60], xmm0
    vaddss  xmm0, xmm9, xmm4
    vsubss  xmm0, xmm14, xmm0
    vmovss  dword ptr [rbp+var_60+4], xmm0
    vaddss  xmm0, xmm6, xmm5
    vmovss  dword ptr [rbp+var_60+8], xmm0
    vaddss  xmm0, xmm3, xmm7
  }
  v562 = 0;
  __asm
  {
    vmovss  dword ptr [rbp+var_50], xmm0
    vsubss  xmm0, xmm6, xmm5
    vmovss  dword ptr [rbp+var_50+4], xmm0
    vaddss  xmm0, xmm9, xmm10
    vsubss  xmm0, xmm14, xmm0
    vmovss  dword ptr [rbp+var_50+8], xmm0
    vxorps  xmm0, xmm0, xmm0
    vmovups [rbp+var_50+0Ch], xmm0
  }
  v566 = _RT0;
  __asm { vxorps  xmm0, xmm13, xmm1 }
  __asm
  {
    vpermilps xmm1, [rbp+var_90], 0
    vshufps xmm0, xmm0, xmm0, 0
    vmulps  xmm0, xmm0, [rbp+var_60]
    vmulps  xmm1, xmm1, [rbp+var_70]
    vsubps  xmm0, xmm0, xmm1
    vshufps xmm1, xmm2, xmm2, 0
    vmulps  xmm1, xmm1, [rbp+var_50]
    vsubps  xmm0, xmm0, xmm1
    vaddps  xmm0, xmm0, [rbp+var_40]
    vmovups [rbp+var_40], xmm0
  }
  fn_a50dc0(a1: v558, a2: v558);
  __asm
  {
    vmovss  xmm0, dword ptr [rbp+var_40+8]
    vxorps  xmm1, xmm0, cs:xmmword_18249F0
    vbroadcastss xmm3, cs:dword_18248A4
    vmulss  xmm0, xmm0, cs:dword_18248B0
    vmovss  xmm13, cs:dword_18248A0
  }
  __asm
  {
    vmovss  dword ptr [rbx+14h], xmm1
    vmovsd  xmm1, qword ptr [rbp+var_40]
    vmovsd  qword ptr [rbx+0Ch], xmm1
    vmulps  xmm1, xmm1, xmm3
    vmovsd  xmm2, qword ptr [rbx]
    vmulps  xmm2, xmm2, xmm3
    vaddps  xmm1, xmm1, xmm2
    vmovss  xmm2, dword ptr [rbx+8]
    vmulss  xmm2, xmm2, cs:dword_18248A4
    vmovlps qword ptr [rbx+18h], xmm1
    vaddss  xmm0, xmm0, xmm2
    vmovss  dword ptr [rbx+20h], xmm0
    vmovss  xmm5, dword ptr [rbx+0E8h]
    vmovss  xmm4, dword ptr [rbx+0E4h]
    vmovss  xmm0, dword ptr [rbx+0F0h]
    vaddss  xmm2, xmm0, dword ptr [rbx+0B8h]
    vmovss  xmm0, dword ptr [rbx+0F4h]
    vaddss  xmm7, xmm0, dword ptr [rbx+0BCh]
    vmovss  xmm3, dword ptr [rbx+0E0h]
    vmovss  xmm6, dword ptr [rbx+0ECh]
    vaddss  xmm0, xmm4, xmm4
    vaddss  xmm1, xmm5, xmm5
    vmulss  xmm8, xmm4, xmm0
    vmulss  xmm9, xmm5, xmm1
    vmulss  xmm10, xmm3, xmm0
    vmulss  xmm11, xmm6, xmm1
    vmulss  xmm14, xmm0, xmm6
    vmulss  xmm15, xmm3, xmm1
    vaddss  xmm12, xmm8, xmm9
    vaddss  xmm0, xmm15, xmm14
    vsubss  xmm12, xmm13, xmm12
    vsubss  xmm13, xmm10, xmm11
    vaddss  xmm10, xmm10, xmm11
    vaddss  xmm11, xmm3, xmm3
    vmulss  xmm12, xmm12, xmm2
    vmulss  xmm13, xmm13, xmm7
    vmulss  xmm10, xmm10, xmm2
    vaddss  xmm12, xmm13, xmm12
    vmovss  xmm13, dword ptr [rbx+0F8h]
    vaddss  xmm13, xmm13, dword ptr [rbx+0C0h]
    vmulss  xmm0, xmm13, xmm0
    vaddss  xmm0, xmm12, xmm0
    vmulss  xmm12, xmm11, xmm3
    vmulss  xmm11, xmm11, xmm6
    vaddss  xmm9, xmm12, xmm9
    vmovaps [rbp+var_90], xmm0
    vmovss  xmm0, cs:dword_18248A0
    vsubss  xmm9, xmm0, xmm9
    vmulss  xmm9, xmm9, xmm7
    vaddss  xmm9, xmm10, xmm9
    vmulss  xmm10, xmm4, xmm1
    vsubss  xmm1, xmm10, xmm11
    vmulss  xmm1, xmm13, xmm1
    vaddss  xmm1, xmm9, xmm1
    vsubss  xmm9, xmm15, xmm14
    vmovaps xmm14, xmm0
    vmulss  xmm2, xmm9, xmm2
    vaddss  xmm9, xmm10, xmm11
    vmovss  xmm10, dword ptr [rbx+0B4h]
    vmulss  xmm7, xmm9, xmm7
    vaddss  xmm2, xmm2, xmm7
    vaddss  xmm7, xmm12, xmm8
    vmulss  xmm11, xmm10, xmm3
    vmovss  xmm8, dword ptr [rbx+0ACh]
    vsubss  xmm7, xmm0, xmm7
    vmulss  xmm7, xmm13, xmm7
    vmulss  xmm13, xmm10, xmm4
    vaddss  xmm2, xmm7, xmm2
    vmovss  xmm7, dword ptr [rbx+0A8h]
    vmulss  xmm9, xmm6, xmm7
    vaddss  xmm9, xmm9, xmm11
    vmovss  xmm11, dword ptr [rbx+0B0h]
    vmulss  xmm12, xmm11, xmm4
    vaddss  xmm9, xmm9, xmm12
    vmulss  xmm12, xmm8, xmm5
    vsubss  xmm9, xmm9, xmm12
    vmulss  xmm12, xmm8, xmm6
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm5, xmm7
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm11, xmm3
    vsubss  xmm12, xmm12, xmm13
    vmulss  xmm13, xmm10, xmm5
    vinsertps xmm9, xmm9, xmm12, 10h
    vmulss  xmm12, xmm11, xmm6
    vmulss  xmm6, xmm10, xmm6
    vaddss  xmm12, xmm13, xmm12
    vmulss  xmm13, xmm8, xmm3
    vmulss  xmm3, xmm3, xmm7
    vsubss  xmm3, xmm6, xmm3
    vbroadcastss xmm6, cs:dword_18248B4
    vaddss  xmm12, xmm12, xmm13
    vmulss  xmm13, xmm4, xmm7
    vmulss  xmm4, xmm8, xmm4
    vsubss  xmm3, xmm3, xmm4
    vmulss  xmm4, xmm11, xmm5
    vsubss  xmm12, xmm12, xmm13
    vmovaps xmm0, xmm6
    vsubss  xmm3, xmm3, xmm4
    vxorps  xmm4, xmm9, xmm6
    vmulss  xmm9, xmm12, cs:dword_1829FC8
    vxorps  xmm1, xmm1, xmm0
    vpermilps xmm0, [rbp+var_90], 0
    vshufps xmm7, xmm4, xmm4, 0F5h
    vxorps  xmm5, xmm12, xmm6
    vaddss  xmm6, xmm4, xmm4
    vshufps xmm1, xmm1, xmm1, 0
    vaddss  xmm8, xmm7, xmm7
    vmulss  xmm10, xmm4, xmm6
    vmulss  xmm6, xmm6, xmm3
    vmulss  xmm11, xmm8, xmm7
    vmulss  xmm12, xmm8, xmm4
    vmulss  xmm8, xmm8, xmm3
    vmulss  xmm5, xmm9, xmm5
    vmulss  xmm4, xmm9, xmm4
    vmulss  xmm3, xmm9, xmm3
    vmulss  xmm7, xmm9, xmm7
    vaddss  xmm9, xmm11, xmm5
    vaddss  xmm15, xmm12, xmm3
    vsubss  xmm13, xmm4, xmm8
    vsubss  xmm3, xmm12, xmm3
    vsubss  xmm9, xmm14, xmm9
    vmovss  dword ptr [rbp+var_70], xmm9
    vmovss  dword ptr [rbp+var_70+4], xmm15
    vmovss  dword ptr [rbp+var_70+8], xmm13
  }
  v558[3] = 0;
  __asm
  {
    vmovss  dword ptr [rbp+var_60], xmm3
    vaddss  xmm3, xmm10, xmm5
    vsubss  xmm3, xmm14, xmm3
    vmovss  dword ptr [rbp+var_60+4], xmm3
    vaddss  xmm3, xmm7, xmm6
    vmovss  dword ptr [rbp+var_60+8], xmm3
    vaddss  xmm3, xmm8, xmm4
  }
  v562 = 0;
  __asm
  {
    vmovss  dword ptr [rbp+var_50], xmm3
    vsubss  xmm3, xmm7, xmm6
    vmovss  dword ptr [rbp+var_50+4], xmm3
    vaddss  xmm3, xmm10, xmm11
    vsubss  xmm3, xmm14, xmm3
    vmovss  dword ptr [rbp+var_50+8], xmm3
    vxorps  xmm3, xmm3, xmm3
    vmovups [rbp+var_50+0Ch], xmm3
  }
  v566 = _RT0;
  __asm
  {
    vmulps  xmm1, xmm1, [rbp+var_60]
    vmulps  xmm0, xmm0, [rbp+var_70]
    vsubps  xmm0, xmm1, xmm0
    vshufps xmm1, xmm2, xmm2, 0
    vmulps  xmm1, xmm1, [rbp+var_50]
    vsubps  xmm0, xmm0, xmm1
    vaddps  xmm0, xmm0, [rbp+var_40]
    vmovups [rbp+var_40], xmm0
  }
  fn_a50dc0(a1: v558, a2: v558);
  __asm
  {
    vmovss  xmm0, dword ptr [rbp+var_40+8]
    vxorps  xmm0, xmm0, cs:xmmword_18249F0
    vmovsd  xmm1, qword ptr [rbp+var_40]
    vmovsd  qword ptr [rbx+24h], xmm1
    vmovss  dword ptr [rbx+2Ch], xmm0
  }
  v531 = (unsigned int)dword_1EE3244;
  if ( dword_1EE3244 > 0 )
  {
    v532 = qword_1EE2828;
    v533 = dword_1EE2E20;
    _R8 = &qword_1EE27D8;
    for ( i = 0; i != v531; ++i )
    {
      v540 = *((_DWORD *)&unk_1EE3250 + 34 * i + 1);
      _R10 = (char *)&unk_1EE3250 + 136 * i + 16;
      if ( v532 == v540 )
      {
        __asm
        {
          vmovups ymm0, cs:ymmword_1EE2FF8+14h
          vmovups ymmword ptr [r10+54h], ymm0
          vmovups ymm0, cs:ymmword_1EE2FF8
          vmovups ymmword ptr [r10+40h], ymm0
          vmovups ymm0, cs:ymmword_1EE2FD8
          vmovups ymmword ptr [r10+20h], ymm0
          vmovups ymm0, cs:ymmword_1EE2FB8
        }
      }
      else
      {
        if ( dword_1EE2FB4 <= 0 )
          continue;
        if ( v533 != v540 )
        {
          v543 = &unk_1EE2EA8;
          v542 = 0;
          while ( (unsigned int)dword_1EE2FB4 - 1LL != v542 )
          {
            ++v542;
            v544 = *v543 == v540;
            v543 += 34;
            if ( v544 )
              goto LABEL_13;
          }
          continue;
        }
        v542 = 0;
LABEL_13:
        _R11 = 136 * v542;
        __asm
        {
          vmovups ymm0, ymmword ptr [r8+r11+654h]
          vmovups ymm1, ymmword ptr [r8+r11+674h]
          vmovups ymm3, ymmword ptr [r8+r11+6A8h]
          vmovups ymm2, ymmword ptr [r8+r11+694h]
          vmovups ymmword ptr [r10+54h], ymm3
          vmovups ymmword ptr [r10+40h], ymm2
          vmovups ymmword ptr [r10+20h], ymm1
        }
      }
      __asm { vmovups ymmword ptr [r10], ymm0 }
    }
  }
  __asm
  {
    vmovups ymm0, cs:ymmword_1EE2FF8+14h
    vmovups ymm1, cs:ymmword_1EE2FF8
    vmovups ymm2, cs:ymmword_1EE2FD8
    vmovups cs:ymmword_1EE3428+14h, ymm0
    vmovups cs:ymmword_1EE3428, ymm1
    vmovups ymm1, cs:ymmword_1EE2FB8
    vmovups cs:ymmword_1EE3408, ymm2
    vmovups cs:ymmword_1EE33E8, ymm1
  }
  return 0x6365786562696C2FLL;
}