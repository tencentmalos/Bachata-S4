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
using OrbisKernelEventFlag = EventFlagInternal*;

struct OrbisKernelEventFlagOptParam {
    size_t size;
};

int PS4_SYSV_ABI sceKernelCreateEventFlag(OrbisKernelEventFlag* ef, const char* pName, u32 attr,
                                          u64 initPattern,
                                          const OrbisKernelEventFlagOptParam* pOptParam) {
    LOG_TRACE(Kernel_Event, "called name = {} attr = {:#x} initPattern = {:#x}", pName, attr,
              initPattern);
    if (ef == nullptr || pName == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (pOptParam || attr > (ORBIS_KERNEL_EVF_ATTR_MULTI | ORBIS_KERNEL_EVF_ATTR_TH_PRIO)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (strlen(pName) >= 32) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    auto thread_mode = EventFlagInternal::ThreadMode::Single;
    auto queue_mode = EventFlagInternal::QueueMode::Fifo;
    switch (attr & 0xfu) {
    case 0x01:
        queue_mode = EventFlagInternal::QueueMode::Fifo;
        break;
    case 0x02:
        queue_mode = EventFlagInternal::QueueMode::ThreadPrio;
        break;
    case 0x00:
        break;
    default:
        UNREACHABLE();
    }

    switch (attr & 0xf0) {
    case 0x10:
        thread_mode = EventFlagInternal::ThreadMode::Single;
        break;
    case 0x20:
        thread_mode = EventFlagInternal::ThreadMode::Multi;
        break;
    case 0x00:
        break;
    default:
        UNREACHABLE();
    }

    *ef = new EventFlagInternal(std::string(pName), thread_mode, queue_mode, initPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEventFlag(OrbisKernelEventFlag ef) {
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    delete ef;
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

int PS4_SYSV_ABI sceKernelClearEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_DEBUG(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Clear(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelCancelEventFlag(OrbisKernelEventFlag ef, u64 setPattern,
                                          int* pNumWaitThreads) {
    LOG_DEBUG(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Cancel(setPattern, pNumWaitThreads);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelSetEventFlag(OrbisKernelEventFlag ef, u64 bitPattern) {
    LOG_TRACE(Kernel_Event, "called");
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    ef->Set(bitPattern);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelPollEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);

    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    if (bitPattern == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto wait = EventFlagInternal::WaitMode::And;
    auto clear = EventFlagInternal::ClearMode::None;
    switch (waitMode & 0xf) {
    case 0x01:
        wait = EventFlagInternal::WaitMode::And;
        break;
    case 0x02:
        wait = EventFlagInternal::WaitMode::Or;
        break;
    default:
        UNREACHABLE();
    }

    switch (waitMode & 0xf0) {
    case 0x00:
        clear = EventFlagInternal::ClearMode::None;
        break;
    case 0x10:
        clear = EventFlagInternal::ClearMode::All;
        break;
    case 0x20:
        clear = EventFlagInternal::ClearMode::Bits;
        break;
    default:
        UNREACHABLE();
    }

    auto result = ef->Poll(bitPattern, wait, clear, pResultPat);

    if (result != ORBIS_OK && result != ORBIS_KERNEL_ERROR_EBUSY) {
        LOG_DEBUG(Kernel_Event, "returned {:#x}", result);
    }

    return result;
}
int PS4_SYSV_ABI sceKernelWaitEventFlag(OrbisKernelEventFlag ef, u64 bitPattern, u32 waitMode,
                                        u64* pResultPat, OrbisKernelUseconds* pTimeout) {
    LOG_DEBUG(Kernel_Event, "called bitPattern = {:#x} waitMode = {:#x}", bitPattern, waitMode);
    if (ef == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }

    if (bitPattern == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto wait = EventFlagInternal::WaitMode::And;
    auto clear = EventFlagInternal::ClearMode::None;
    switch (waitMode & 0xf) {
    case 0x01:
        wait = EventFlagInternal::WaitMode::And;
        break;
    case 0x02:
        wait = EventFlagInternal::WaitMode::Or;
        break;
    default:
        UNREACHABLE();
    }

    switch (waitMode & 0xf0) {
    case 0x00:
        clear = EventFlagInternal::ClearMode::None;
        break;
    case 0x10:
        clear = EventFlagInternal::ClearMode::All;
        break;
    case 0x20:
        clear = EventFlagInternal::ClearMode::Bits;
        break;
    default:
        UNREACHABLE();
    }

    const int result = ef->Wait(bitPattern, wait, clear, pResultPat, pTimeout, g_curthread->attr.prio);
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
