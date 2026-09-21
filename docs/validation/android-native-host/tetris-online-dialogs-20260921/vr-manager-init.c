__int64 __fastcall sub_3C50990(__int64 a1)
{
  int Module; // eax
  __int64 v17; // rax
  __int64 v18; // r14
  _QWORD *v24; // rbx
  __int64 v25; // rbx
  __int64 result; // rax
  _QWORD v27[2]; // [rsp+0h] [rbp-140h] BYREF
  int v28; // [rsp+10h] [rbp-130h] BYREF
  _BYTE v29[60]; // [rsp+14h] [rbp-12Ch]
  __int64 v30; // [rsp+50h] [rbp-F0h] BYREF
  _QWORD v31[7]; // [rsp+58h] [rbp-E8h] BYREF
  _QWORD v32[3]; // [rsp+90h] [rbp-B0h] BYREF
  _QWORD v33[5]; // [rsp+A8h] [rbp-98h] BYREF
  __int64 v36; // [rsp+110h] [rbp-30h]

  _R15 = a1;
  v36 = 0x6365786562696C2FLL;
  Module = plt_sceSysmoduleLoadModule(a1: 237);
  *(_DWORD *)(_R15 + 2832) = Module;
  if ( Module >= 0 && (unsigned int)plt_sceSysmoduleLoadModule(a1: 164) == 0 )
  {
    plt_sceCommonDialogInitialize();
    *(_DWORD *)(_R15 + 2836) = plt_sceCameraOpen(a1: 255, a2: 0, a3: 0, a4: 0);
    LODWORD(v30) = 0;
    v28 = 0;
    v33[0] = &v30;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3456, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 1;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3456, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 2;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3456, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 3;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3456, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 0;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3536, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 1;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3536, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 2;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3536, a2: v33, a3: 0);
    v33[0] = &v30;
    LODWORD(v30) = 3;
    v28 = 0;
    v33[1] = &v28;
    sub_3C55910(a1: _R15 + 3536, a2: v33, a3: 0);
    __asm { vmovups xmm0, cs:xmmword_4E10290 }
    __asm
    {
      vxorps  xmm1, xmm1, xmm1
      vmovups [rbp+var_D0], ymm1
    }
    *(_OWORD *)&v31[5] = *(_OWORD *)&_RT0.m256i_u64[2];
    __asm
    {
      vmovups ymmword ptr [rbp-0E8h], ymm1
      vmovups [rbp+var_110], ymm1
      vmovups ymmword ptr [rbp-12Ch], ymm1
    }
    v28 = 64;
    v30 = 0x6400000040LL;
    __asm { vmovups xmmword ptr [rbp+var_D0], xmm0 }
    if ( (unsigned int)plt_sceVrTrackerQueryMemory(a1: &v30, a2: &v28, a3: *(double *)&_XMM0, a4: *(double *)&_XMM1) == 0 )
    {
      __asm
      {
        vxorps  xmm0, xmm0, xmm0
        vmovups ymmword ptr [r15+0EB0h], ymm0
        vmovups ymmword ptr [r15+0E98h], ymm0
        vmovups ymmword ptr [r15+0E78h], ymm0
        vmovups ymmword ptr [r15+0E58h], ymm0
      }
      *(_DWORD *)(_R15 + 3664) = 128;
      _RAX = &v31[3];
      *(_DWORD *)(_R15 + 3668) = HIDWORD(v30);
      __asm
      {
        vmovups ymm0, ymmword ptr [rax]
        vmovups ymmword ptr [r15+0E78h], ymm0
      }
      sub_FEC130(a1: v33, a2: *(unsigned int *)v29, a3: *(unsigned int *)&v29[4], a4: 1, a5: *(double *)&_XMM0);
      __asm
      {
        vmovups xmm0, xmmword ptr [rbp+var_98]
        vmovups xmm1, xmmword ptr [rbp+var_98+0Ch]
      }
      __asm
      {
        vmovups xmmword ptr [r15+1164h], xmm1
        vmovups xmmword ptr [r15+1158h], xmm0
      }
      *(_QWORD *)(_R15 + 3736) = *(_QWORD *)(_R15 + 4440);
      *(_DWORD *)(_R15 + 3744) = *(_DWORD *)v29;
      *(_DWORD *)(_R15 + 3748) = *(_DWORD *)&v29[4];
      sub_FEC130(a1: v33, a2: *(unsigned int *)&v29[8], a3: *(unsigned int *)&v29[12], a4: 2, a5: *(double *)&_XMM0);
      __asm
      {
        vmovups xmm0, xmmword ptr [rbp+var_98]
        vmovups xmm1, xmmword ptr [rbp+var_98+0Ch]
        vmovups xmmword ptr [r15+1184h], xmm1
        vmovups xmmword ptr [r15+1178h], xmm0
      }
      *(_QWORD *)(_R15 + 3752) = *(_QWORD *)(_R15 + 4472);
      *(_DWORD *)(_R15 + 3760) = *(_DWORD *)&v29[8];
      *(_DWORD *)(_R15 + 3764) = *(_DWORD *)&v29[12];
      v17 = plt_memalign(a1: *(unsigned int *)&v29[20], a2: *(unsigned int *)&v29[16]);
      *(_QWORD *)(_R15 + 3768) = v17;
      *(_DWORD *)(_R15 + 3776) = *(_DWORD *)&v29[16];
      *(_DWORD *)(_R15 + 3780) = *(_DWORD *)&v29[20];
      if ( v17 != 0 )
        plt_memset(a1: v17, a2: 0);
      *(_QWORD *)(_R15 + 3784) = 6;
      if ( (unsigned int)plt_sceVrTrackerInit(a1: _R15 + 3664) == 0 )
      {
        v18 = 0;
        plt_memset(a1: _R15 + 3796, a2: 0);
        *(_DWORD *)(_R15 + 3792) = 648;
        *(_DWORD *)(_R15 + 3808) = 0;
        *(double *)&_XMM0 = plt_memset(a1: _R15 + 2864, a2: 0);
        __asm { vxorps  xmm0, xmm0, xmm0 }
        *(_QWORD *)(_R15 + 2856) = 0x1100000248LL;
        __asm
        {
          vmovups [rbp+var_50], ymm0
          vmovups [rbp+var_70], ymm0
          vmovups ymmword ptr [rbp+var_98+8], ymm0
        }
        v33[0] = 0x500000068LL;
        plt_sceCameraSetConfig(a1: *(unsigned int *)(_R15 + 2836), a2: v33, a3: *(double *)&_XMM0);
        v27[0] = 0x100000010LL;
        v27[1] = 0;
        *(double *)&_XMM0 = plt_sceCameraSetVideoSync(a1: *(unsigned int *)(_R15 + 2836), a2: v27);
        __asm
        {
          vxorps  xmm0, xmm0, xmm0
          vmovups [rbp+var_B0], xmm0
        }
        HIDWORD(v32[1]) = HIDWORD(_RT0);
        v32[2] = 0;
        v32[0] = 0xF00000018LL;
        LODWORD(v32[1]) = 15;
        plt_sceCameraStart(a1: *(unsigned int *)(_R15 + 2836), a2: v32);
        plt_scePthreadCreate(a1: _R15 + 3624, a2: 0, a3: sub_3C51320, a4: _R15, a5: "FPS4Tracker::CameraThread");
        plt_scePthreadSetprio(a1: *(_QWORD *)(_R15 + 3624), a2: 256);
        *(_DWORD *)(_R15 + 3616) = plt_sceCameraIsAttached(a1: 0);
        *(_QWORD *)(_R15 + 2840) = 0;
        v33[0] = 0;
        LODWORD(v33[1]) = 0;
        v24 = (_QWORD *)sub_10110(a1: v33, a2: 48);
        *v24 = &unk_5EE3048;
        v24[2] = sub_1015FA0();
        *v24 = &unk_6470360;
        v24[3] = _R15;
        v24[4] = sub_3C513B0;
        v24[5] = 0;
        if ( LODWORD(v33[1]) != 0 && v33[0] != 0 )
          v18 = sub_1C080(a1: &xmmword_6BC3F48, a2: v33);
        sub_B8E850(a1: v33);
        *(_QWORD *)(_R15 + 3440) = v18;
        v25 = sub_1015FD0();
        if ( byte_6AAACA0 == 0 && (unsigned int)plt___cxa_guard_acquire(a1: &byte_6AAACA0) != 0 )
        {
          sub_11FB040(a1: &qword_6AAAC98, a2: L"MotionTrackingSystemManagement", a3: 1);
          plt___cxa_guard_release(a1: &byte_6AAACA0);
        }
        (*(void (__fastcall **)(__int64, __int64, __int64))(*(_QWORD *)v25 + 32LL))(
          a1: v25,
          a2: qword_6AAAC98,
          a3: _R15 + 8);
      }
    }
  }
  result = 0x6365786562696C2FLL;
  if ( v36 != 0x6365786562696C2FLL )
  {
    plt___stack_chk_fail();
    BUG();
  }
  return result;
}