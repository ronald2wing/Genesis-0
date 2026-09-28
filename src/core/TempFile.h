// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Private scratch files. A fixed name in the shared temp directory is a file
// another local user can pre-create, read, or symlink elsewhere; mkstemp gives
// a fresh name, created exclusively with mode 0600, so what the host decodes
// into it stays its own.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

#include <cstdlib>

namespace genesis::core {

// A freshly created, empty, 0600 file named `<temp dir>/<prefix>XXXXXX` with
// the X's filled in; the caller owns it and removes it. Nullopt when the temp
// directory refuses the create.
inline std::optional<std::filesystem::path> make_private_temp_file(std::string_view prefix)
{
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return std::nullopt;
    }
    std::string pattern = (dir / (std::string(prefix) + "XXXXXX")).string();
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
        return std::nullopt;
    }
    ::close(fd);
    return std::filesystem::path(pattern);
}

} // namespace genesis::core
