// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace Libraries::Kernel {
struct OrbisKernelTitleWorkaround {
    s32 version;
    s32 align;
    u64 ids[2];
};

struct OrbisKernelAppInfo {
    s32 app_id;
    s32 mmap_flags;
    s32 attribute_exe;
    s32 attribute2;
    char cusa_name[10];
    u8 debug_level;
    u8 slv_flags;
    u8 mini_app_dmem_flags;
    u8 render_mode;
    u8 mdbg_out;
    u8 required_hdcp_type;
    u64 preload_prx_flags;
    s32 attribute1;
    s32 has_param_sfo;
    OrbisKernelTitleWorkaround title_workaround;
};

static_assert(sizeof(OrbisKernelTitleWorkaround) == 24);
static_assert(sizeof(OrbisKernelAppInfo) == 72);
} // namespace Libraries::Kernel
