// SPDX-License-Identifier: GPL-2.0-or-later
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    char raw[PATH_MAX], binary[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, binary)) return 1;
    char* slash = strrchr(binary, '/');
    if (!slash || (size_t)(slash - binary) + sizeof("/shadps4") > sizeof(binary)) return 1;
    strcpy(slash, "/shadps4");
    // Use the standard macOS shadPS4 profile, independent of Finder's cwd.
    if (chdir("/") != 0) return 1;
    const char* args[] = {binary, "--online-store", NULL};
    execv(binary, (char* const*)args);
    perror("Cannot start shadPS4");
    return 1;
}
