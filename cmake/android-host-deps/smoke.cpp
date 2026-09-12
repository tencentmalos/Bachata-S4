// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
extern "C" int shadps4_host_dependencies_check();
int main() {
    return shadps4_host_dependencies_check();
}
