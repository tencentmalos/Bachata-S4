// SPDX-License-Identifier: GPL-2.0-or-later
// Deliberately NOT linked to the host DSO: startup loading masks initial-exec TLS.
#include <cstdio>
#include <dlfcn.h>
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "HOST_DLOPEN_FAIL: %s\n", dlerror());
        return 1;
    }
    if (dlclose(handle) != 0)
        return 1;
    std::puts("HOST_DLOPEN_PASS");
}
