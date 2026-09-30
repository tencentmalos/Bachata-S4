__int64 __fastcall fn_e02c0(__int64 a1, int a2, __m128 _XMM0)
{
  unsigned int DeviceInfo; // r14d
  _BOOL4 v7; // r13d
  __m256 v19; // [rsp+0h] [rbp-90h]
  __int64 v21; // [rsp+48h] [rbp-48h]
  __int128 v22; // [rsp+50h] [rbp-40h] BYREF
  __int64 v23; // [rsp+60h] [rbp-30h]

  v23 = loc_0;
  if ( a2 < 0 )
  {
    return (unsigned int)-2131820540;
  }
  else
  {
    _R15 = a1;
    __asm
    {
      vxorps  xmm0, xmm0, xmm0
      vmovups [rbp+var_40], xmm0
    }
    DeviceInfo = sceMoveGetDeviceInfo(a1: (unsigned int)a2, a2: &v22);
    if ( DeviceInfo == 0 )
    {
      v7 = *(_DWORD *)(_R15 + 192) == 2;
      v21 = *(int *)(_R15 + 176);
      fn_82f20(a1: &dword_2D41D0, a2: v21, a3: &dword_2D78C0, a4: *(_DWORD *)(_R15 + 192) == 2);
      __asm
      {
        vmovsd  xmm0, qword ptr [rbp+var_40+4]
        vmovups xmm1, cs:xmmword_135740
        vmovss  xmm2, dword ptr [rbp+var_40+0Ch]
      }
      _RDI = &dword_2D41D0;
      _RAX = 3 * v21;
      __asm
      {
        vxorps  xmm0, xmm0, xmm1
        vxorps  xmm1, xmm2, xmm1
        vmovlps qword ptr [rdi+rax*4+7F4h], xmm0
        vmovss  dword ptr [rdi+rax*4+7FCh], xmm1
      }
      __asm
      {
        vmovups ymm0, ymmword ptr [r15+0C8h]
        vmovups ymm1, ymmword ptr [r15+0E8h]
        vmovups [rsp+90h+var_70], ymm1
        vmovups [rsp+90h+var_90], ymm0
        vxorps  xmm0, xmm0, xmm0
      }
      fn_83e40(
        a1: (unsigned int)&dword_2D41D0,
        a2: v21,
        a3: a2,
        a4: v7,
        a5: unk_174320 == 0 || unk_174320 == 6,
        a6: (unsigned int)&dword_2D78C0,
        a7: SLOBYTE(v19.m256_f32[0]));
    }
  }
  return DeviceInfo;
}