// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/AssetPaths.h"

#include <cstdlib>
#include <system_error>

namespace genesis::workspace {

namespace {

// `path` when it names an existing directory, otherwise empty.
std::filesystem::path existing_dir(const std::filesystem::path &path)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        return path;
    }
    return { };
}

// Walks up from `dir` looking for a directory named `assets`. Stops at the
// filesystem root.
std::filesystem::path walk_up_for_assets(std::filesystem::path dir)
{
    std::error_code ec;
    dir = std::filesystem::absolute(dir, ec);
    if (ec) {
        return { };
    }
    while (true) {
        const std::filesystem::path candidate = dir / "assets";
        if (std::filesystem::is_directory(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return { };
        }
        dir = parent;
    }
}

// The directory the running executable lives in, when it can be found. Only
// Linux has a filesystem probe for this; elsewhere it is empty.
std::filesystem::path executable_dir()
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

} // namespace

std::filesystem::path asset_root()
{
    if (const char *env = std::getenv("GENESIS_ASSET_DIR")) {
        if (const std::filesystem::path found = existing_dir(env); !found.empty()) {
            return found;
        }
    }
#ifdef GENESIS_ASSET_DIR
    if (const std::filesystem::path found = existing_dir(GENESIS_ASSET_DIR); !found.empty()) {
        return found;
    }
#endif
    if (const std::filesystem::path found = walk_up_for_assets(std::filesystem::current_path());
        !found.empty()) {
        return found;
    }
    return walk_up_for_assets(executable_dir());
}

} // namespace genesis::workspace
