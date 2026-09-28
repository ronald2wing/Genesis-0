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

std::filesystem::path installed_share_root(const std::filesystem::path &exe_dir)
{
    if (exe_dir.empty()) {
        return { };
    }
    return exe_dir.parent_path() / "share" / "genesis-0";
}

std::filesystem::path resolve_asset_root(const std::filesystem::path &exe_dir,
                                         const std::filesystem::path &cwd, const char *env_override,
                                         const char *compile_time_dir)
{
    if (env_override != nullptr) {
        if (const std::filesystem::path found = existing_dir(env_override); !found.empty()) {
            return found;
        }
    }
    if (compile_time_dir != nullptr) {
        if (const std::filesystem::path found = existing_dir(compile_time_dir); !found.empty()) {
            return found;
        }
    }
    if (const std::filesystem::path found = walk_up_for_assets(cwd); !found.empty()) {
        return found;
    }
    if (const std::filesystem::path share = installed_share_root(exe_dir); !share.empty()) {
        if (const std::filesystem::path found = existing_dir(share / "assets"); !found.empty()) {
            return found;
        }
    }
    return walk_up_for_assets(exe_dir);
}

std::filesystem::path asset_root()
{
#ifdef GENESIS_ASSET_DIR
    const char *compile_time_dir = GENESIS_ASSET_DIR;
#else
    const char *compile_time_dir = nullptr;
#endif
    return resolve_asset_root(executable_dir(), std::filesystem::current_path(),
                              std::getenv("GENESIS_ASSET_DIR"), compile_time_dir);
}

} // namespace genesis::workspace
