// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The real Common::SetCurrentThreadName reaches into kernel thread state via
// Libraries::Kernel::g_curthread, which would drag the whole kernel into any
// test that touches a ZArchive backend (its reader owns a worker thread).
// Thread names are not what these tests are about, so stub them out.
// GetCurrentThreadName already lives in common_stub.cpp.

namespace Common {

void SetCurrentThreadName(const char*) {}

void SetThreadName(void*, const char*) {}

} // namespace Common
