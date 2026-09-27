// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <condition_variable>
#include <cstring>
#include <list>
#include <mutex>
#include <semaphore>

#include "core/libraries/kernel/sync/semaphore.h"
#include "core/libraries/kernel/sync/counting_semaphore.h"
#include "core/libraries/kernel/sync/kernel_semaphore.h"
#include "core/libraries/kernel/sync/object_table.h"

#include "common/logging/log.h"

#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/wait_platform.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

constexpr s32 ORBIS_KERNEL_SEM_VALUE_MAX = 0x7FFFFFFF;

// sem_t: the semaphore is the shared Sync::CountingSemaphore the Android host runtime also uses;
// this layer supplies the pthread cancellation-point behaviour of the wait.
struct PthreadSem {
    explicit PthreadSem(u32 value) : sem{value} {}
    Sync::CountingSemaphore sem;
};

static s32 PthreadSemWait(PthreadSem* sem, const OrbisKernelTimespec* abstime, u64 usec = 0) {
    // libkernel checks and decrements the value before validating a timeout.
    if (sem->sem.TryWait()) {
        return 0;
    }
    if (abstime != nullptr && abstime != THR_RELTIME &&
        (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1'000'000'000)) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    ASSERT_MSG(g_curthread != nullptr, "POSIX semaphore wait requires a guest pthread");
    PthreadTestCancel();
    CancellationPointWait wait{g_curthread, abstime, usec};
    const int result = sem->sem.Wait(wait);
    if (result == 0) {
        return 0;
    }
    // Interrupted means cancellation is pending at this cancellation point; a timed-out wait
    // still acts on a cancellation that arrived meanwhile.
    PthreadTestCancel();
    *__Error() = result == POSIX_EINTR ? POSIX_EINTR : POSIX_ETIMEDOUT;
    return -1;
}

// Orbis kernel semaphores: the semaphore itself is the shared Sync::KernelSemaphore the Android
// host runtime also uses; this layer maps the guest ids and supplies the waiting thread's priority.
using KernelSemaphore = Sync::KernelSemaphore;
using OrbisKernelSema = u32;

static Sync::ObjectTable<KernelSemaphore> orbis_sems;

s32 PS4_SYSV_ABI sceKernelCreateSema(OrbisKernelSema* sem, const char* pName, u32 attr,
                                     s32 initCount, s32 maxCount, const void* pOptParam) {
    if (!sem || !pName || attr > 2 || initCount < 0 || maxCount <= 0 || initCount > maxCount ||
        pOptParam) {
        LOG_ERROR(Lib_Kernel, "Semaphore creation parameters are invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (std::strlen(pName) >= 32) {
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }
    const auto id =
        orbis_sems.Insert(std::make_shared<KernelSemaphore>(pName, initCount, maxCount, attr == 1));
    if (!id) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    *sem = *id;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelWaitSema(OrbisKernelSema sem, s32 needCount, u32* pTimeout) {
    const auto object = orbis_sems.Find(sem);
    if (!object) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    // Priority-ordered semaphores queue lower priority values first.
    const u32 priority = g_curthread ? static_cast<u32>(g_curthread->attr.prio) : 0;
    return object->Wait(needCount, priority, pTimeout);
}

s32 PS4_SYSV_ABI sceKernelSignalSema(OrbisKernelSema sem, s32 signalCount) {
    const auto object = orbis_sems.Find(sem);
    if (!object) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return object->Signal(signalCount);
}

s32 PS4_SYSV_ABI sceKernelPollSema(OrbisKernelSema sem, s32 needCount) {
    const auto object = orbis_sems.Find(sem);
    if (!object) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return object->Poll(needCount);
}

s32 PS4_SYSV_ABI sceKernelCancelSema(OrbisKernelSema sem, s32 setCount, s32* pNumWaitThreads) {
    const auto object = orbis_sems.Find(sem);
    if (!object) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    return object->Cancel(setCount, pNumWaitThreads);
}

s32 PS4_SYSV_ABI sceKernelDeleteSema(OrbisKernelSema sem) {
    // Waiters still hold the object: they return EACCES after it left the table.
    const auto object = orbis_sems.Erase(sem);
    if (!object) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    object->Delete();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI posix_sem_init(PthreadSem** sem, s32 /*pshared*/, u32 value) {
    if (value > ORBIS_KERNEL_SEM_VALUE_MAX) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (sem != nullptr) {
        *sem = new PthreadSem(value);
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_destroy(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    delete *sem;
    *sem = nullptr;
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_wait(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    return PthreadSemWait(*sem, nullptr);
}

s32 PS4_SYSV_ABI posix_sem_trywait(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (!(*sem)->sem.TryWait()) {
        *__Error() = POSIX_EAGAIN;
        return -1;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_timedwait(PthreadSem** sem, const OrbisKernelTimespec* t) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    return PthreadSemWait(*sem, t);
}

s32 PS4_SYSV_ABI posix_sem_reltimedwait_np(PthreadSem** sem, u32 usec) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    return PthreadSemWait(*sem, THR_RELTIME, usec);
}

s32 PS4_SYSV_ABI posix_sem_post(PthreadSem** sem) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    ScopedPthreadCritical critical{g_curthread};
    if (const int error = (*sem)->sem.Post()) {
        *__Error() = error;
        return -1;
    }
    return 0;
}

s32 PS4_SYSV_ABI posix_sem_getvalue(PthreadSem** sem, s32* sval) {
    if (sem == nullptr || *sem == nullptr) {
        *__Error() = POSIX_EINVAL;
        return -1;
    }
    if (sval) {
        *sval = static_cast<s32>((*sem)->sem.Value());
    }
    return 0;
}

s32 PS4_SYSV_ABI scePthreadSemInit(PthreadSem** sem, s32 flag, u32 value, const char* name) {
    if (flag != 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    s32 ret = posix_sem_init(sem, 0, value);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemDestroy(PthreadSem** sem) {
    s32 ret = posix_sem_destroy(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemWait(PthreadSem** sem) {
    s32 ret = posix_sem_wait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTrywait(PthreadSem** sem) {
    s32 ret = posix_sem_trywait(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemTimedwait(PthreadSem** sem, u32 usec) {
    s32 ret = posix_sem_reltimedwait_np(sem, usec);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemPost(PthreadSem** sem) {
    s32 ret = posix_sem_post(sem);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI scePthreadSemGetvalue(PthreadSem** sem, s32* sval) {
    s32 ret = posix_sem_getvalue(sem, sval);
    if (ret != 0) {
        return ErrnoToSceKernelError(*__Error());
    }

    return ORBIS_OK;
}

void RegisterSemaphore(Core::Loader::SymbolsResolver* sym) {
    // Orbis
    LIB_FUNCTION("188x57JYp0g", "libkernel", 1, "libkernel", sceKernelCreateSema);
    LIB_FUNCTION("Zxa0VhQVTsk", "libkernel", 1, "libkernel", sceKernelWaitSema);
    LIB_FUNCTION("4czppHBiriw", "libkernel", 1, "libkernel", sceKernelSignalSema);
    LIB_FUNCTION("12wOHk8ywb0", "libkernel", 1, "libkernel", sceKernelPollSema);
    LIB_FUNCTION("4DM06U2BNEY", "libkernel", 1, "libkernel", sceKernelCancelSema);
    LIB_FUNCTION("R1Jvn8bSCW8", "libkernel", 1, "libkernel", sceKernelDeleteSema);

    // Posix
    LIB_FUNCTION("pDuPEf3m4fI", "libScePosix", 1, "libkernel", posix_sem_init);
    LIB_FUNCTION("cDW233RAwWo", "libScePosix", 1, "libkernel", posix_sem_destroy);
    LIB_FUNCTION("YCV5dGGBcCo", "libScePosix", 1, "libkernel", posix_sem_wait);
    LIB_FUNCTION("WBWzsRifCEA", "libScePosix", 1, "libkernel", posix_sem_trywait);
    LIB_FUNCTION("w5IHyvahg-o", "libScePosix", 1, "libkernel", posix_sem_timedwait);
    LIB_FUNCTION("4SbrhCozqQU", "libScePosix", 1, "libkernel", posix_sem_reltimedwait_np);
    LIB_FUNCTION("IKP8typ0QUk", "libScePosix", 1, "libkernel", posix_sem_post);
    LIB_FUNCTION("Bq+LRV-N6Hk", "libScePosix", 1, "libkernel", posix_sem_getvalue);

    LIB_FUNCTION("pDuPEf3m4fI", "libkernel", 1, "libkernel", posix_sem_init);
    LIB_FUNCTION("cDW233RAwWo", "libkernel", 1, "libkernel", posix_sem_destroy);
    LIB_FUNCTION("YCV5dGGBcCo", "libkernel", 1, "libkernel", posix_sem_wait);
    LIB_FUNCTION("WBWzsRifCEA", "libkernel", 1, "libkernel", posix_sem_trywait);
    LIB_FUNCTION("w5IHyvahg-o", "libkernel", 1, "libkernel", posix_sem_timedwait);
    LIB_FUNCTION("4SbrhCozqQU", "libkernel", 1, "libkernel", posix_sem_reltimedwait_np);
    LIB_FUNCTION("IKP8typ0QUk", "libkernel", 1, "libkernel", posix_sem_post);
    LIB_FUNCTION("Bq+LRV-N6Hk", "libkernel", 1, "libkernel", posix_sem_getvalue);

    LIB_FUNCTION("GEnUkDZoUwY", "libkernel", 1, "libkernel", scePthreadSemInit);
    LIB_FUNCTION("Vwc+L05e6oE", "libkernel", 1, "libkernel", scePthreadSemDestroy);
    LIB_FUNCTION("C36iRE0F5sE", "libkernel", 1, "libkernel", scePthreadSemWait);
    LIB_FUNCTION("H2a+IN9TP0E", "libkernel", 1, "libkernel", scePthreadSemTrywait);
    LIB_FUNCTION("fjN6NQHhK8k", "libkernel", 1, "libkernel", scePthreadSemTimedwait);
    LIB_FUNCTION("aishVAiFaYM", "libkernel", 1, "libkernel", scePthreadSemPost);
    LIB_FUNCTION("DjpBvGlaWbQ", "libkernel", 1, "libkernel", scePthreadSemGetvalue);
}

} // namespace Libraries::Kernel
