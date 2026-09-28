// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Filesystem fixtures shared by the test suites. Each helper was carried
// verbatim by several suites before being gathered here.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace genesis::test {

// The file's bytes with newlines stripped, so a JSON fixture compares equal
// regardless of how the writer wrapped it.
inline std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        out += line;
    }
    return out;
}

// A file of `size` bytes, back-dated `age_seconds` so a cache sweep sees it as
// old. Returns the path so a caller can chain.
inline std::filesystem::path write_file(const std::filesystem::path &path, std::uintmax_t size,
                                        int age_seconds)
{
    std::ofstream out(path, std::ios::binary);
    for (std::uintmax_t i = 0; i < size; ++i) {
        out.put('x');
    }
    out.close();
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(path, now - std::chrono::seconds(age_seconds));
    return path;
}

// A text file written verbatim.
inline void write_text_file(const std::filesystem::path &path, const std::string &text)
{
    std::ofstream out(path);
    out << text;
}

} // namespace genesis::test
