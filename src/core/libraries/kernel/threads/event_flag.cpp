// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "pthread.h"
#include "event_flag_state.h"
#include "core/libraries/kernel/sync/event_flags.h"

namespace Libraries::Kernel {

constexpr int ORBIS_KERNEL_EVF_ATTR_TH_FIFO = 0x01;
constexpr int ORBIS_KERNEL_EVF_ATTR_TH_PRIO = 0x02;
constexpr int ORBIS_KERNEL_EVF_ATTR_SINGLE = 0x10;
constexpr int ORBIS_KERNEL_EVF_ATTR_MULTI = 0x20;

constexpr int ORBIS_KERNEL_EVF_WAITMODE_AND = 0x01;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_OR = 0x02;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_CLEAR_ALL = 0x10;
constexpr int ORBIS_KERNEL_EVF_WAITMODE_CLEAR_PAT = 0x20;

using EventFlagInternal = EventFlagState;

using OrbisKernelUseconds = u32;
// Guest handles are ids in the shared event flag table: a flag deleted while threads wait on it
// stays alive until they leave it.
using OrbisKernelEventFlag = u64;

struct OrbisKernelEventFlagOptParam {
    size_t size;
};

static Sync::EventFlagTable event_flags;

int PS4_SYSV_ABI sceKernelCreateEventFlag(OrbisKernelEventFlag* ef, const char* pName, u32 attr,
                                          u64 initPattern,
                                          const OrbisKernelEventFlagOptParam* pOptParam) {
    LOG_TRACE(Kernel_Event, "called name = {} attr = {:#x} initPattern = {:#x}", pName, attr,
              initPattern);
    if (ef == nullptr || pName == nullptr || pOptParam) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const auto attributes = Sync::DecodeEventFlagAttributes(attr);
    if (!attributes) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (strlen(pName) >= 32) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }
    const auto id = event_flags.Insert(std::make_shared<EventFlagInternal>(
        std::string(pName), attributes->thread_mode, attributes->queue_mode, initPattern));
    if (!id) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    *ef = *id;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEventFlag(OrbisKernelEventFlag ef) {
    const auto flag = ef > UINT32_MAX ? nullptr : event_flags.Erase(static_cast<u32>(ef));
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    // Waiters are released with the deletion result before the last owner lets go.
    flag->Delete();
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelOpenEventFlag() {
    LOG_ERROR(Kernel_Event, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelCloseEventFlag() {
    LOG_ERROR(Kernel_Event, "(STUBBED) called");
    return ORBIS_OK;
}

static std::shared_ptr<EventFlagInternal> FindEventFlag(OrbisKernelEventFlag ef) {
    return ef > UINT32_MAX ? nullptr : event_flags.Find(static_cast<u32>(ef));
}

int PS4_SYSV_ABI sceKernelClearEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_DEBUG(Kernel_Event, "called");
    const auto flag = FindEventFlag(ef);
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    flag->Clear(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelCancelEventFlag(OrbisKernelEventFlag ef, u64 setPattern,
                                          int* pNumWaitThreads) {
    LOG_DEBUG(Kernel_Event, "called");
    const auto flag = FindEventFlag(ef);
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    flag->Cancel(setPattern, pNumWaitThreads);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelSetEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_TRACE(Kernel_Event, "called");
    const auto flag = FindEventFlag(ef);
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    flag->Set(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelPollEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);
    const auto flag = FindEventFlag(ef);
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    const auto mode = Sync::DecodeEventFlagWaitMode(waitMode);
    if (bitPattern == 0 || !mode) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const auto result = flag->Poll(bitPattern, mode->wait, mode->clear, pResultPat);
    if (result != ORBIS_OK && result != ORBIS_KERNEL_ERROR_EBUSY) {
        LOG_DEBUG(Kernel_Event, "returned {:#x}", result);
    }
    return result;
}

int PS4_SYSV_ABI sceKernelWaitEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat, OrbisKernelUseconds* pTimeout) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);
    const auto flag = FindEventFlag(ef);
    if (!flag) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    const auto mode = Sync::DecodeEventFlagWaitMode(waitMode);
    if (bitPattern == 0 || !mode) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const int result = flag->Wait(bitPattern, mode->wait, mode->clear, pResultPat, pTimeout,
                                  g_curthread->attr.prio);
    if (result != ORBIS_OK && result != ORBIS_KERNEL_ERROR_ETIMEDOUT) {
        LOG_DEBUG(Kernel_Event, "returned {:#x}", result);
    }
    return result;
}

void RegisterKernelEventFlag(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("PZku4ZrXJqg", "libkernel", 1, "libkernel", sceKernelCancelEventFlag);
    LIB_FUNCTION("7uhBFWRAS60", "libkernel", 1, "libkernel", sceKernelClearEventFlag);
    LIB_FUNCTION("s9-RaxukuzQ", "libkernel", 1, "libkernel", sceKernelCloseEventFlag);
    LIB_FUNCTION("BpFoboUJoZU", "libkernel", 1, "libkernel", sceKernelCreateEventFlag);
    LIB_FUNCTION("8mql9OcQnd4", "libkernel", 1, "libkernel", sceKernelDeleteEventFlag);
    LIB_FUNCTION("1vDaenmJtyA", "libkernel", 1, "libkernel", sceKernelOpenEventFlag);
    LIB_FUNCTION("9lvj5DjHZiA", "libkernel", 1, "libkernel", sceKernelPollEventFlag);
    LIB_FUNCTION("IOnSvHzqu6A", "libkernel", 1, "libkernel", sceKernelSetEventFlag);
    LIB_FUNCTION("JTvBflhYazQ", "libkernel", 1, "libkernel", sceKernelWaitEventFlag);
}

} // namespace Libraries::Kernel
