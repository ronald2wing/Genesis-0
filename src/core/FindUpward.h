// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The one walk-up-the-tree file locator. Suites run from build/ and tools run
// from the repo root both reach checked-in files through it, so the search
// rule ("nearest enclosing directory that has it") lives once.

#pragma once

#include <filesystem>
#include <system_error>

namespace genesis::core {

// The nearest `start`-or-ancestor directory's `relative`, walking toward the
// filesystem root; empty when no ancestor has it. `relative` may name a file
// or a directory.
inline std::filesystem::path find_upward(const std::filesystem::path &relative,
                                         std::filesystem::path start)
{
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) {
        return { };
    }
    while (true) {
        const std::filesystem::path candidate = start / relative;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = start.parent_path();
        if (parent == start) {
            return { };
        }
        start = parent;
    }
}

// The same walk, starting at the current working directory.
inline std::filesystem::path find_upward(const std::filesystem::path &relative)
{
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        return { };
    }
    return find_upward(relative, cwd);
}

} // namespace genesis::core
