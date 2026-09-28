// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Whole-file binary read for the on-disk caches. The file is sized first, so a
// missing or truncated file is refused before a buffer is handed back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace genesis::core {

inline bool read_binary_file(const std::filesystem::path &path, std::vector<std::uint8_t> &bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (size == 0) {
        return true;
    }
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    return in.good() || in.eof();
}

} // namespace genesis::core
