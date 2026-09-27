// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/format.h>

#include "common/assert.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/threads/thread_state.h"
#include "core/libraries/kernel/threads/wait_platform.h"
#include "core/libraries/libs.h"

namespace Libraries::Kernel {

static std::mutex CondStaticLock;

#define THR_COND_INITIALIZER ((PthreadCond*)nullptr)
#define THR_COND_DESTROYED ((PthreadCond*)1)

static constexpr PthreadCondAttr PthreadCondattrDefault = {
    .c_pshared = 0,
    .c_clockid = ClockId::Realtime,
};

static int CondInit(PthreadCondT* cond, const PthreadCondAttrT* cond_attr, const char* name) {
    auto* cvp = new (std::nothrow) PthreadCond{};
    if (cvp == nullptr) {
        return POSIX_ENOMEM;
    }

    if (name) {
        cvp->name = name;
    } else {
        static std::atomic<int> CondId{0};
        cvp->name = fmt::format("Cond{}", CondId.fetch_add(1));
    }

    if (cond_attr == nullptr || *cond_attr == nullptr) {
        cvp->clock_id = ClockId::Realtime;
    } else {
        // if ((*cond_attr)->c_pshared) {
        //     cvp->flags |= USYNC_PROCESS_SHARED;
        // }
        cvp->clock_id = (*cond_attr)->c_clockid;
    }
    *cond = cvp;
    return 0;
}

static int InitStatic(Pthread* thread, PthreadCondT* cond) {
    std::scoped_lock lk{CondStaticLock};
    if (*cond == nullptr) {
        return CondInit(cond, nullptr, nullptr);
    }
    return 0;
}

#define CHECK_AND_INIT_COND                                                                        \
    if (cvp = *cond; cvp <= THR_COND_DESTROYED) [[unlikely]] {                                     \
        if (cvp == THR_COND_INITIALIZER) {                                                         \
            int ret;                                                                               \
            ret = InitStatic(g_curthread, cond);                                                   \
            if (ret)                                                                               \
                return (ret);                                                                      \
        } else if (cvp == THR_COND_DESTROYED) {                                                    \
            return POSIX_EINVAL;                                                                   \
        }                                                                                          \
        cvp = *cond;                                                                               \
    }

int PS4_SYSV_ABI posix_pthread_cond_init(PthreadCondT* cond, const PthreadCondAttrT* cond_attr) {
    *cond = nullptr;
    return CondInit(cond, cond_attr, nullptr);
}

int PS4_SYSV_ABI scePthreadCondInit(PthreadCondT* cond, const PthreadCondAttrT* cond_attr,
                                    const char* name) {
    *cond = nullptr;
    return CondInit(cond, cond_attr, name);
}

int PS4_SYSV_ABI posix_pthread_cond_destroy(PthreadCondT* cond) {
    PthreadCond* cvp = *cond;
    if (cvp == THR_COND_INITIALIZER) {
        return 0;
    }
    if (cvp == THR_COND_DESTROYED) {
        return POSIX_EINVAL;
    }
    // Waiters still queued or reacquiring their mutex use the object.
    if (cvp->cv.Busy()) {
        return POSIX_EBUSY;
    }
    *cond = THR_COND_DESTROYED;
    delete cvp;
    return 0;
}

namespace {

// The desktop side of a condition wait: a pthread cancellation point that sleeps on the thread's
// wake semaphore and hands its mutex over under the condition lock.
class CondWait final : public CancellationPointWait {
public:
    CondWait(Pthread* curthread_, PthreadMutex* mp_, const OrbisKernelTimespec* abstime,
             u64 usec, ClockId clock_id)
        : CancellationPointWait{curthread_, abstime, usec, clock_id}, mp{mp_} {}

    u64 Owner() const {
        return reinterpret_cast<u64>(curthread);
    }
    const void* Context() const {
        return mp;
    }
    int ReleaseMutex() {
        // The thread is about to sleep: wakes deferred to this unlock are issued when it does.
        curthread->will_sleep = true;
        curthread->mutex_obj = mp;
        return mp->CvUnlock(&recurse);
    }
    int ReacquireMutex() {
        curthread->mutex_obj = nullptr;
        // Wakes deferred to the unlock above must not be lost if the thread never slept.
        curthread->will_sleep = false;
        if (curthread->nwaiter_defer > 0) {
            curthread->WakeAll();
        }
        return mp->CvLock(recurse);
    }

private:
    PthreadMutex* mp;
    int recurse{};
};

// A notifier holding the waiter's mutex defers its wake until it unlocks the mutex: waking it
// now would only make it block on that mutex (libthr's deferred wakeup).
struct DeferToUnlock {
    Pthread* curthread;
    bool operator()(const Sync::ConditionVariable::Waiter& waiter) const {
        auto* mp = static_cast<PthreadMutex*>(const_cast<void*>(waiter.context));
        if (mp == nullptr || curthread == nullptr || mp->m_owner.load() != curthread) {
            return false;
        }
        if (curthread->nwaiter_defer >= Pthread::MaxDeferWaiters) {
            curthread->WakeAll();
        }
        Pthread* thread = static_cast<const ThreadWaitSlot&>(*waiter.slot).Thread();
        curthread->defer_waiters[curthread->nwaiter_defer++] = &thread->wake_sema;
        mp->m_flags |= PthreadMutexFlags::Deferred;
        return true;
    }
};

} // Anonymous namespace

int PthreadCond::Wait(PthreadMutexT* mutex, const OrbisKernelTimespec* abstime, u64 usec) {
    PthreadMutex* mp = *mutex;
    if (const int error = mp->IsOwned(g_curthread); error != 0) {
        return error;
    }
    Pthread* curthread = g_curthread;
    PthreadTestCancel();
    CondWait wait{curthread, mp, abstime, usec, clock_id};
    const int error = cv.Wait(wait);
    if (error == POSIX_EINTR) {
        // Cancellation is pending at this cancellation point; the mutex is held again.
        PthreadTestCancel();
        return 0;
    }
    PthreadCancelInterrupt();
    return error;
}

int PS4_SYSV_ABI posix_pthread_cond_wait(PthreadCondT* cond, PthreadMutexT* mutex) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    return cvp->Wait(mutex, nullptr);
}

int PS4_SYSV_ABI posix_pthread_cond_timedwait(PthreadCondT* cond, PthreadMutexT* mutex,
                                              const OrbisKernelTimespec* abstime) {
    if (abstime == nullptr || abstime->tv_sec < 0 || abstime->tv_nsec < 0 ||
        abstime->tv_nsec >= 1000000000) {
        return POSIX_EINVAL;
    }

    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    return cvp->Wait(mutex, abstime);
}

int PS4_SYSV_ABI posix_pthread_cond_reltimedwait_np(PthreadCondT* cond, PthreadMutexT* mutex,
                                                    u64 usec) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    return cvp->Wait(mutex, THR_RELTIME, usec);
}

int PthreadCond::Signal(Pthread* thread) {
    Pthread* curthread = g_curthread;
    if (thread) {
        auto* thread_state = ThrState::Instance();
        int ret = thread_state->FindThread(thread, false);
        if (ret != ORBIS_OK) {
            return ret;
        }
        thread->lock->unlock();
    }
    ScopedPthreadCritical critical{curthread};
    const bool selected = cv.Signal(reinterpret_cast<u64>(thread), DeferToUnlock{curthread});
    return !selected && thread != nullptr ? 1 : 0;
}

int PthreadCond::Broadcast() {
    Pthread* curthread = g_curthread;
    ScopedPthreadCritical critical{curthread};
    cv.Broadcast(DeferToUnlock{curthread});
    return 0;
}

int PS4_SYSV_ABI posix_pthread_cond_signal(PthreadCondT* cond) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    return cvp->Signal(nullptr);
}

int PS4_SYSV_ABI posix_pthread_cond_signalto_np(PthreadCondT* cond, Pthread* thread) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    return cvp->Signal(thread);
}

int PS4_SYSV_ABI posix_pthread_cond_broadcast(PthreadCondT* cond) {
    PthreadCond* cvp{};
    CHECK_AND_INIT_COND
    cvp->Broadcast();
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_init(PthreadCondAttrT* attr) {
    auto* pattr = new (std::nothrow) PthreadCondAttr{};
    if (pattr == nullptr) {
        return POSIX_ENOMEM;
    }
    memcpy(pattr, &PthreadCondattrDefault, sizeof(PthreadCondAttr));
    *attr = pattr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_destroy(PthreadCondAttrT* attr) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    delete *attr;
    *attr = nullptr;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_getclock(const PthreadCondAttrT* attr, ClockId* clock_id) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    *clock_id = (*attr)->c_clockid;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_setclock(PthreadCondAttrT* attr, ClockId clock_id) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    if (clock_id != ClockId::Realtime && clock_id != ClockId::Virtual &&
        clock_id != ClockId::Prof && clock_id != ClockId::Monotonic) {
        return POSIX_EINVAL;
    }
    (*attr)->c_clockid = clock_id;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_getpshared(const PthreadCondAttrT* attr, int* pshared) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    *pshared = 0;
    return 0;
}

int PS4_SYSV_ABI posix_pthread_condattr_setpshared(PthreadCondAttrT* attr, int pshared) {
    if (attr == nullptr || *attr == nullptr) {
        return POSIX_EINVAL;
    }
    if (pshared != 0) {
        return POSIX_EINVAL;
    }
    return 0;
}

void RegisterCond(Core::Loader::SymbolsResolver* sym) {
    // Posix
    LIB_FUNCTION("mKoTx03HRWA", "libScePosix", 1, "libkernel", posix_pthread_condattr_init);
    LIB_FUNCTION("3BpP850hBT4", "libScePosix", 1, "libkernel", posix_pthread_condattr_setpshared);
    LIB_FUNCTION("EjllaAqAPZo", "libScePosix", 1, "libkernel", posix_pthread_condattr_setclock);
    LIB_FUNCTION("h0qUqSuOmC8", "libScePosix", 1, "libkernel", posix_pthread_condattr_getpshared);
    LIB_FUNCTION("cTDYxTUNPhM", "libScePosix", 1, "libkernel", posix_pthread_condattr_getclock);
    LIB_FUNCTION("dJcuQVn6-Iw", "libScePosix", 1, "libkernel", posix_pthread_condattr_destroy);
    LIB_FUNCTION("0TyVk4MSLt0", "libScePosix", 1, "libkernel", posix_pthread_cond_init);
    LIB_FUNCTION("K953PF5u6Pc", "libScePosix", 1, "libkernel", posix_pthread_cond_reltimedwait_np);
    LIB_FUNCTION("27bAgiJmOh0", "libScePosix", 1, "libkernel", posix_pthread_cond_timedwait);
    LIB_FUNCTION("Op8TBGY5KHg", "libScePosix", 1, "libkernel", posix_pthread_cond_wait);
    LIB_FUNCTION("2MOy+rUfuhQ", "libScePosix", 1, "libkernel", posix_pthread_cond_signal);
    LIB_FUNCTION("CI6Qy73ae10", "libScePosix", 1, "libkernel", posix_pthread_cond_signalto_np);
    LIB_FUNCTION("mkx2fVhNMsg", "libScePosix", 1, "libkernel", posix_pthread_cond_broadcast);
    LIB_FUNCTION("RXXqi4CtF8w", "libScePosix", 1, "libkernel", posix_pthread_cond_destroy);

    // Posix-Kernel
    LIB_FUNCTION("mKoTx03HRWA", "libkernel", 1, "libkernel", posix_pthread_condattr_init);
    LIB_FUNCTION("3BpP850hBT4", "libkernel", 1, "libkernel", posix_pthread_condattr_setpshared);
    LIB_FUNCTION("EjllaAqAPZo", "libkernel", 1, "libkernel", posix_pthread_condattr_setclock);
    LIB_FUNCTION("h0qUqSuOmC8", "libkernel", 1, "libkernel", posix_pthread_condattr_getpshared);
    LIB_FUNCTION("cTDYxTUNPhM", "libkernel", 1, "libkernel", posix_pthread_condattr_getclock);
    LIB_FUNCTION("dJcuQVn6-Iw", "libkernel", 1, "libkernel", posix_pthread_condattr_destroy);
    LIB_FUNCTION("0TyVk4MSLt0", "libkernel", 1, "libkernel", posix_pthread_cond_init);
    LIB_FUNCTION("K953PF5u6Pc", "libkernel", 1, "libkernel", posix_pthread_cond_reltimedwait_np);
    LIB_FUNCTION("27bAgiJmOh0", "libkernel", 1, "libkernel", posix_pthread_cond_timedwait);
    LIB_FUNCTION("Op8TBGY5KHg", "libkernel", 1, "libkernel", posix_pthread_cond_wait);
    LIB_FUNCTION("2MOy+rUfuhQ", "libkernel", 1, "libkernel", posix_pthread_cond_signal);
    LIB_FUNCTION("CI6Qy73ae10", "libkernel", 1, "libkernel", posix_pthread_cond_signalto_np);
    LIB_FUNCTION("mkx2fVhNMsg", "libkernel", 1, "libkernel", posix_pthread_cond_broadcast);
    LIB_FUNCTION("RXXqi4CtF8w", "libkernel", 1, "libkernel", posix_pthread_cond_destroy);

    // Orbis
    LIB_FUNCTION("m5-2bsNfv7s", "libkernel", 1, "libkernel", ORBIS(posix_pthread_condattr_init));
    LIB_FUNCTION("6xMew9+rZwI", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_setpshared));
    LIB_FUNCTION("c-bxj027czs", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_setclock));
    LIB_FUNCTION("Dn-DRWi9t54", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_getpshared));
    LIB_FUNCTION("6qM3kO5S3Oo", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_condattr_getclock));
    LIB_FUNCTION("waPcxYiR3WA", "libkernel", 1, "libkernel", ORBIS(posix_pthread_condattr_destroy));
    LIB_FUNCTION("2Tb92quprl0", "libkernel", 1, "libkernel", ORBIS(scePthreadCondInit));
    LIB_FUNCTION("BmMjYxmew1w", "libkernel", 1, "libkernel",
                 ORBIS(posix_pthread_cond_reltimedwait_np));
    LIB_FUNCTION("WKAXJ4XBPQ4", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_wait));
    LIB_FUNCTION("kDh-NfxgMtE", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_signal));
    LIB_FUNCTION("o69RpYO-Mu0", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_signalto_np));
    LIB_FUNCTION("JGgj7Uvrl+A", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_broadcast));
    LIB_FUNCTION("g+PZd2hiacg", "libkernel", 1, "libkernel", ORBIS(posix_pthread_cond_destroy));
}

} // namespace Libraries::Kernel
