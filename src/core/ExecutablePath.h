// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The directory the running executable lives in, when it can be found. Only
// Linux has a filesystem probe for this; elsewhere it is empty. The extensions
// seed and the asset-path resolver both need it, so it lives once.

#pragma once

#include <filesystem>
#include <system_error>

namespace genesis::core {

inline std::filesystem::path executable_dir()
{
#if defined(__linux__)
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return exe.parent_path();
    }
#endif
    return { };
}

} // namespace genesis::core
