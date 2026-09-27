// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>

#include "common/elf_info.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

static std::mutex RwlockStaticLock;
static s32 sdk_version;

#define THR_RWLOCK_INITIALIZER ((PthreadRwlock*)NULL)
#define THR_RWLOCK_DESTROYED ((PthreadRwlock*)1)

#define CHECK_AND_INIT_RWLOCK                                                                      \
    if (prwlock = (*rwlock); prwlock <= THR_RWLOCK_DESTROYED) [[unlikely]] {                       \
        if (prwlock == THR_RWLOCK_INITIALIZER) {                                                   \
            int ret;                                                                               \
            ret = InitStatic(g_curthread, rwlock);                                                 \
            if (ret)                                                                               \
                return (ret);                                                                      \
        } else if (prwlock == THR_RWLOCK_DESTROYED) {                                              \
            return POSIX_EINVAL;                                                                   \
        }                                                                                          \
        prwlock = *rwlock;                                                                         \
    }

static int RwlockInit(PthreadRwlockT* rwlock, const PthreadRwlockAttrT* attr) {
    u32 type = 0;
    if (attr != nullptr && *attr != nullptr) {
        if ((*attr)->type > 2) {
            if (sdk_version >= Common::ElfInfo::FW_450) {
                return POSIX_EINVAL;
            }
        } else {
            type = static_cast<u32>((*attr)->type);
        }
    }
    auto* prwlock = new (std::nothrow) PthreadRwlock{type};
    if (prwlock == nullptr) {
        return POSIX_ENOMEM;
    }
    *rwlock = prwlock;
    return 0;
}

// The calling thread's identity as a lock owner.
static u64 RwlockOwner() {
    return reinterpret_cast<u64>(g_curthread);
}

static int RwlockAcquire(PthreadRwlock* prwlock, bool write, const OrbisKernelTimespec* abstime) {
    const u64 owner = RwlockOwner();
    Sync::ParkerWait no_wait{{}};
    // POSIX: the timeout need not be validated when the lock can be taken at once.
    if (prwlock->lock.Lock(owner, write, true, no_wait) == 0) {
        return 0;
    }
    if (abstime && (abstime->tv_nsec >= 1000000000 || abstime->tv_nsec < 0)) [[unlikely]] {
        return POSIX_EINVAL;
    }
    Sync::Deadline deadline;
    if (abstime != nullptr) {
        const auto left = abstime->TimePoint() - std::chrono::system_clock::now();
        deadline = std::chrono::steady_clock::now() +
                   std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::max(left, decltype(left)::zero()));
    }
    Sync::ParkerWait wait{{}, deadline};
    return prwlock->lock.Lock(owner, write, false, wait);
}

int PS4_SYSV_ABI posix_pthread_rwlock_destroy(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock = *rwlock;
    if (prwlock == THR_RWLOCK_INITIALIZER) {
        return 0;
    }
    if (prwlock == THR_RWLOCK_DESTROYED) {
        return POSIX_EINVAL;
    }
    if (prwlock->lock.Busy()) {
        return POSIX_EBUSY;
    }
    *rwlock = THR_RWLOCK_DESTROYED;
    delete prwlock;
    return 0;
}

static int InitStatic(Pthread* thread, PthreadRwlockT* rwlock) {
    std::scoped_lock lk{RwlockStaticLock};
    if (*rwlock == THR_RWLOCK_INITIALIZER) {
        auto* prwlock = new (std::nothrow) PthreadRwlock{};
        if (prwlock == nullptr) {
            return POSIX_ENOMEM;
        }
        *rwlock = prwlock;
    }
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlock_init(PthreadRwlockT* rwlock, const PthreadRwlockAttrT* attr) {
    *rwlock = nullptr;
    return RwlockInit(rwlock, attr);
}

int PthreadRwlock::Rdlock(const OrbisKernelTimespec* abstime) {
    return RwlockAcquire(this, false, abstime);
}

int PthreadRwlock::Wrlock(const OrbisKernelTimespec* abstime) {
    return RwlockAcquire(this, true, abstime);
}

int PS4_SYSV_ABI posix_pthread_rwlock_rdlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Rdlock(nullptr);
}

int PS4_SYSV_ABI posix_pthread_rwlock_timedrdlock(PthreadRwlockT* rwlock,
                                                  const OrbisKernelTimespec* abstime) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Rdlock(abstime);
}

int PS4_SYSV_ABI posix_pthread_rwlock_tryrdlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    Sync::ParkerWait no_wait{{}};
    return prwlock->lock.Lock(RwlockOwner(), false, true, no_wait);
}

int PS4_SYSV_ABI posix_pthread_rwlock_trywrlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    Sync::ParkerWait no_wait{{}};
    return prwlock->lock.Lock(RwlockOwner(), true, true, no_wait);
}

int PS4_SYSV_ABI posix_pthread_rwlock_wrlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Wrlock(nullptr);
}

int PS4_SYSV_ABI posix_pthread_rwlock_timedwrlock(PthreadRwlockT* rwlock,
                                                  const OrbisKernelTimespec* abstime) {
    PthreadRwlockT prwlock{};
    CHECK_AND_INIT_RWLOCK
    return prwlock->Wrlock(abstime);
}

int PS4_SYSV_ABI posix_pthread_rwlock_unlock(PthreadRwlockT* rwlock) {
    PthreadRwlockT prwlock = *rwlock;
    if (prwlock <= THR_RWLOCK_DESTROYED) [[unlikely]] {
        return POSIX_EINVAL;
    }
    return prwlock->lock.Unlock(RwlockOwner());
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_destroy(PthreadRwlockAttrT* rwlockattr) {
    if (rwlockattr == nullptr) {
        return POSIX_EINVAL;
    }
    PthreadRwlockAttrT prwlockattr = *rwlockattr;
    if (prwlockattr == nullptr) {
        return POSIX_EINVAL;
    }

    delete prwlockattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_getpshared(const PthreadRwlockAttrT* rwlockattr,
                                                     int* pshared) {
    *pshared = (*rwlockattr)->pshared;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_gettype_np(const PthreadRwlockAttrT* rwlockattr,
                                                     int* type) {
    if (rwlockattr == nullptr || *rwlockattr == nullptr || (*rwlockattr)->type > 2) {
        return POSIX_EINVAL;
    }
    *type = (*rwlockattr)->type;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_init(PthreadRwlockAttrT* rwlockattr) {
    if (rwlockattr == nullptr) {
        return POSIX_EINVAL;
    }

    auto prwlockattr = new (std::nothrow) PthreadRwlockAttr{};
    if (prwlockattr == nullptr) {
        return POSIX_ENOMEM;
    }

    prwlockattr->pshared = 0;
    *rwlockattr = prwlockattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_settype_np(const PthreadRwlockAttrT* rwlockattr,
                                                     int type) {
    if (rwlockattr == nullptr || *rwlockattr == nullptr || type < 0 || type > 2) {
        return POSIX_EINVAL;
    }
    (*rwlockattr)->type = type;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_rwlockattr_setpshared(PthreadRwlockAttrT* rwlockattr, int pshared) {
    /* Only PTHREAD_PROCESS_PRIVATE is supported. */
    if (pshared != 0) {
        return POSIX_EINVAL;
    }

    (*rwlockattr)->pshared = pshared;
    return 0;
}

void RegisterRwlock(Core::Loader::SymbolsResolver* sym) {
    ASSERT_MSG(sceKernelGetCompiledSdkVersion(&sdk_version) == 0, "Failed to get SDK version");

    // Posix-Kernel
    LIB_FUNCTION("1471ajPzxh0", "libkernel", 1, "libkernel", posix_pthread_rwlock_destroy);
    LIB_FUNCTION("ytQULN-nhL4", "libkernel", 1, "libkernel", posix_pthread_rwlock_init);
    LIB_FUNCTION("iGjsr1WAtI0", "libkernel", 1, "libkernel", posix_pthread_rwlock_rdlock);
    LIB_FUNCTION("lb8lnYo-o7k", "libkernel", 1, "libkernel", posix_pthread_rwlock_timedrdlock);
    LIB_FUNCTION("9zklzAl9CGM", "libkernel", 1, "libkernel", posix_pthread_rwlock_timedwrlock);
    LIB_FUNCTION("SFxTMOfuCkE", "libkernel", 1, "libkernel", posix_pthread_rwlock_tryrdlock);
    LIB_FUNCTION("XhWHn6P5R7U", "libkernel", 1, "libkernel", posix_pthread_rwlock_trywrlock);
    LIB_FUNCTION("EgmLo6EWgso", "libkernel", 1, "libkernel", posix_pthread_rwlock_unlock);
    LIB_FUNCTION("sIlRvQqsN2Y", "libkernel", 1, "libkernel", posix_pthread_rwlock_wrlock);
    LIB_FUNCTION("qsdmgXjqSgk", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_destroy);
    LIB_FUNCTION("VqEMuCv-qHY", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_getpshared);
    LIB_FUNCTION("l+bG5fsYkhg", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_gettype_np);
    LIB_FUNCTION("xFebsA4YsFI", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_init);
    LIB_FUNCTION("OuKg+kRDD7U", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_setpshared);
    LIB_FUNCTION("8NuOHiTr1Vw", "libkernel", 1, "libkernel", posix_pthread_rwlockattr_settype_np);

    // Posix
    LIB_FUNCTION("1471ajPzxh0", "libScePosix", 1, "libkernel", posix_pthread_rwlock_destroy);
    LIB_FUNCTION("ytQULN-nhL4", "libScePosix", 1, "libkernel", posix_pthread_rwlock_init);
    LIB_FUNCTION("iGjsr1WAtI0", "libScePosix", 1, "libkernel", posix_pthread_rwlock_rdlock);
    LIB_FUNCTION("lb8lnYo-o7k", "libScePosix", 1, "libkernel", posix_pthread_rwlock_timedrdlock);
    LIB_FUNCTION("9zklzAl9CGM", "libScePosix", 1, "libkernel", posix_pthread_rwlock_timedwrlock);
    LIB_FUNCTION("SFxTMOfuCkE", "libScePosix", 1, "libkernel", posix_pthread_rwlock_tryrdlock);
    LIB_FUNCTION("XhWHn6P5R7U", "libScePosix", 1, "libkernel", posix_pthread_rwlock_trywrlock);
    LIB_FUNCTION("EgmLo6EWgso", "libScePosix", 1, "libkernel", posix_pthread_rwlock_unlock);
    LIB_FUNCTION("sIlRvQqsN2Y", "libScePosix", 1, "libkernel", posix_pthread_rwlock_wrlock);
    LIB_FUNCTION("qsdmgXjqSgk", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_destroy);
    LIB_FUNCTION("VqEMuCv-qHY", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_getpshared);
    LIB_FUNCTION("l+bG5fsYkhg", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_gettype_np);
    LIB_FUNCTION("xFebsA4YsFI", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_init);
    LIB_FUNCTION("OuKg+kRDD7U", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_setpshared);
    LIB_FUNCTION("8NuOHiTr1Vw", "libScePosix", 1, "libkernel", posix_pthread_rwlockattr_settype_np);

    // Orbis
    LIB_FUNCTION("i2ifZ3fS2fo", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_destroy));
    LIB_FUNCTION("LcOZBHGqbFk", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_getpshared));
    LIB_FUNCTION("Kyls1ChFyrc", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_gettype_np));
    LIB_FUNCTION("yOfGg-I1ZII", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlockattr_init));
    LIB_FUNCTION("-ZvQH18j10c", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_setpshared));
    LIB_FUNCTION("h-OifiouBd8", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlockattr_settype_np));
    LIB_FUNCTION("BB+kb08Tl9A", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_destroy));
    LIB_FUNCTION("6ULAa0fq4jA", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_init));
    LIB_FUNCTION("Ox9i0c7L5w0", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_rdlock));
    LIB_FUNCTION("iPtZRWICjrM", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlock_timedrdlock));
    LIB_FUNCTION("adh--6nIqTk", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_rwlock_timedwrlock));
    LIB_FUNCTION("XD3mDeybCnk", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_tryrdlock));
    LIB_FUNCTION("bIHoZCTomsI", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_trywrlock));
    LIB_FUNCTION("+L98PIbGttk", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_unlock));
    LIB_FUNCTION("mqdNorrB+gI", "libkernel", 1, "libkernel", ORBIS(posix_pthread_rwlock_wrlock));
}

} // namespace Libraries::Kernel
