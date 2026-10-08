// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "core/host_runtime/guest_login.h"

using Core::HostRuntime::GuestLogin;

int main() {
    unsigned checks{}, failures{};
    auto check = [&](bool pass) {
        ++checks;
        if (!pass) {
            ++failures;
            std::printf("FAIL %u\n", checks);
        }
    };
    GuestLogin a, b;
    check(a.Invoke("HAiWUEwEfGo") == 0);
    check(a.Invoke("2rc+egSfb5A") == 0);
    check(a.Invoke("vMQJRUKsf3U") == 0x81340001);
    check(a.Invoke("aR8+Hvghm0E") == 0x813a0001);
    check(a.Invoke("T9-NuSHAvcQ") == 0);
    check(a.Invoke("T9-NuSHAvcQ") == 0x813a0002);
    // Service initialization must not initialize the independent dialog.
    check(a.Invoke("HAiWUEwEfGo") == 0);
    check(a.Invoke("qP-EvQRl2Hc") == 0);
    check(a.Invoke("qP-EvQRl2Hc") == 0x81340002);
    check(a.Invoke("HAiWUEwEfGo") == 1);
    check(a.Invoke("2rc+egSfb5A") == 1);
    check(a.Invoke("aR8+Hvghm0E") == 0);
    check(a.Invoke("HAiWUEwEfGo") == 1);
    check(a.Invoke("vMQJRUKsf3U") == 0);
    check(a.Invoke("HAiWUEwEfGo") == 0);
    check(a.Invoke("qP-EvQRl2Hc") == 0);
    check(a.Invoke("T9-NuSHAvcQ") == 0);
    check(b.Invoke("HAiWUEwEfGo") == 0);
    check(b.Invoke("aR8+Hvghm0E") == 0x813a0001);
    check(b.Invoke("qP-EvQRl2Hc") == 0);
    check(GuestLogin::Admits("T9-NuSHAvcQ", "#libSceLoginService#1#libSceLoginService#Function"));
    check(!GuestLogin::Admits("T9-NuSHAvcQ", "#libkernel#1#libkernel#Function"));
    check(!GuestLogin::Admits("qP-EvQRl2Hc", "#libSceSigninDialog#1#libSceSigninDialog#Function"));
    check(!GuestLogin::IsNid("S1Uxf-lgJ-c")); // no unchecked device request
    check(!GuestLogin::IsNid("S56ra1+Tymg")); // no fabricated login result
    std::printf("guest_login_tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
