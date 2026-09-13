// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "common/scope_exit.h"
#include <limits>
#include "common/singleton.h"
#include "core/file_sys/directories/base_directory.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/orbis_error.h"

namespace Core::Directories {

BaseDirectory::BaseDirectory() = default;

BaseDirectory::~BaseDirectory() = default;

s64 BaseDirectory::readv(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt) {
    s64 bytes_read = 0;
    for (s32 i = 0; i < iovcnt; i++) {
        const s64 result = read(iov[i].iov_base, iov[i].iov_len);
        if (result < 0) {
            return result;
        }
        bytes_read += result;
    }
    return bytes_read;
}

s64 BaseDirectory::preadv(const Libraries::Kernel::OrbisKernelIovec* iov, s32 iovcnt, s64 offset) {
    if (offset < 0 || iovcnt < 0)
        return ORBIS_KERNEL_ERROR_EINVAL;
    const s64 old_file_pointer = file_offset;
    SCOPE_EXIT { file_offset = old_file_pointer; };
    file_offset = offset;
    return readv(iov, iovcnt);
}

s64 BaseDirectory::lseek(s64 offset, s32 whence) {
    if (whence < 0 || whence > 2 || directory_size > u64(std::numeric_limits<s64>::max()))
        return ORBIS_KERNEL_ERROR_EINVAL;
    const s64 base = whence == 0 ? 0 : whence == 1 ? file_offset : s64(directory_size);
    if ((offset > 0 && base > std::numeric_limits<s64>::max() - offset) ||
        (offset < 0 && offset < -base))
        return ORBIS_KERNEL_ERROR_EINVAL;
    const s64 file_offset_new = base + offset;

    file_offset = file_offset_new;
    return file_offset;
}

} // namespace Core::Directories
