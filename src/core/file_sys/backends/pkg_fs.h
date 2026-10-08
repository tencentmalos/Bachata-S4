// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/file_format/pkg_reader.h"
#include "core/file_sys/ifile.h"

namespace Core::FileSys {

class PkgBackend final : public IBackend {
public:
    explicit PkgBackend(std::filesystem::path path);
    bool IsOpen() const {
        return reader != nullptr;
    }
    bool Exists(std::string_view path) override;
    bool IsDirectory(std::string_view path) override;
    std::unique_ptr<IFile> Open(std::string_view path, Common::FS::FileAccessMode mode) override;
    std::unique_ptr<IDirectory> OpenDir(std::string_view path) override;
    bool IsReadOnly() const override {
        return true;
    }
    std::filesystem::path RootPath() const override {
        return path;
    }
    std::optional<std::vector<u8>> ReadFile(std::string_view path) const override;

private:
    size_t Find(std::string_view path) const;
    std::filesystem::path path;
    std::shared_ptr<FileFormat::PkgReader> reader;
};

} // namespace Core::FileSys
