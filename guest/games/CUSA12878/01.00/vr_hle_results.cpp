// SPDX-License-Identifier: GPL-2.0-or-later
#include "shad_guest.h"
extern "C" int original_HmdGetDeviceInformation(void*);
extern "C" int original_HmdOpen(int,int,int,void*);
extern "C" int original_CameraIsAttached(int);
extern "C" int patch_HmdGetDeviceInformation(void* info) {
    const auto result=original_HmdGetDeviceInformation(info);
    shad_sdk_log(58,result);
    if(!result && info) {
        const auto* fields=static_cast<const int*>(info);
        shad_sdk_log(56,fields[0]); shad_sdk_log(57,fields[1]);
    }
    return result;
}
extern "C" int patch_HmdOpen(int user,int type,int index,void* param) {
    shad_sdk_log(59,user);
    const auto result=original_HmdOpen(user,type,index,param);
    shad_sdk_log(60,result);
    return result;
}
extern "C" int patch_CameraIsAttached(int index) {
    const auto result=original_CameraIsAttached(index);
    shad_sdk_log(61,result);
    return result;
}

extern "C" int original_ReprojectionInitialize(const void*,unsigned int,const void*);
extern "C" int original_CameraGetFrameData(int,void*);
extern "C" int patch_ReprojectionInitialize(const void* param,unsigned int mode,const void* reserved) {
    shad_sdk_log(62,mode);
    const auto result=original_ReprojectionInitialize(param,mode,reserved);
    shad_sdk_log(63,result);
    return result;
}
namespace { shad_u64 frame_calls; }
extern "C" int patch_CameraGetFrameData(int handle,void* output) {
    const auto result=original_CameraGetFrameData(handle,output);
    const auto n=__atomic_add_fetch(&frame_calls,1,__ATOMIC_RELAXED);
    if(n<=4 || (n&1023)==0) shad_sdk_log(64,result);
    return result;
}
