// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Seed.h"

#include <optional>
#include <system_error>
#include <utility>

#include "extensions/Install.h"
#include "extensions/ExtensionManifest.h"
#include "extensions/Origins.h"
#include "extensions/RemovedList.h"
#include "extensions/Trust.h"
#include "workspace/AssetPaths.h"

namespace genesis::extensions {

namespace {

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

// The builtin id from the directory's extension.toml, or null when it holds
// no valid manifest. Validation is the manifest loader's own, so a non-null id
// means the manifest passed.
std::optional<std::string> builtin_id(const std::filesystem::path &dir)
{
    if (std::filesystem::is_regular_file(dir / "extension.toml")) {
        const NativeLoadResult loaded = load_native_manifest_file(dir / "extension.toml");
        if (loaded.manifest) {
            return loaded.manifest->id;
        }
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path resolve_builtin_root(const std::filesystem::path &exe_dir,
                                           const char *compile_time_dir)
{
    std::error_code ec;
    if (compile_time_dir != nullptr) {
        if (std::filesystem::is_directory(compile_time_dir, ec) && !ec) {
            return compile_time_dir;
        }
        ec.clear();
    }
    if (exe_dir.empty()) {
        return { };
    }
    const std::filesystem::path beside = exe_dir / "extensions" / "builtin";
    if (std::filesystem::is_directory(beside, ec) && !ec) {
        return beside;
    }
    ec.clear();
    const std::filesystem::path share = genesis::workspace::installed_share_root(exe_dir);
    if (share.empty()) {
        return { };
    }
    const std::filesystem::path installed = share / "extensions" / "builtin";
    if (std::filesystem::is_directory(installed, ec) && !ec) {
        return installed;
    }
    return { };
}

std::filesystem::path builtin_root()
{
#ifdef GENESIS_BUILTIN_EXTENSIONS_DIR
    const char *compile_time_dir = GENESIS_BUILTIN_EXTENSIONS_DIR;
#else
    const char *compile_time_dir = nullptr;
#endif
    return resolve_builtin_root(executable_dir(), compile_time_dir);
}

std::vector<std::string> seed(const std::filesystem::path &store_root,
                              const std::filesystem::path &builtin_root)
{
    std::vector<std::string> problems;

    std::error_code ec;
    if (!std::filesystem::is_directory(builtin_root, ec) || ec) {
        problems.push_back("no builtin extension source at " + builtin_root.string());
        return problems;
    }

    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator it(builtin_root, ec);
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::directory_entry entry = *it;
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) {
            continue; // the README and any stray file are not builtins
        }
        const std::string name = entry.path().filename().string();
        if (name.starts_with('.')) {
            continue; // hidden/staging directories are never builtins
        }

        const std::optional<std::string> id = builtin_id(entry.path());
        if (!id) {
            problems.push_back("skipping " + name + ": no valid manifest");
            continue;
        }

        // A tombstoned id stays gone: the user removed it, so the seed must not
        // silently re-add it on the next startup.
        if (removed(store_root, *id)) {
            continue;
        }
        // Already installed (any version): leave it untouched, so re-seeding is
        // idempotent and never downgrades or duplicates.
        std::error_code dir_ec;
        if (std::filesystem::is_directory(store_root / *id, dir_ec)) {
            continue;
        }

        const InstallResult result =
                install_extension(entry.path(), store_root, Origin::Builtin,
                                  InstallSource{ .type = SourceType::Builtin });
        if (!result.ok()) {
            for (const std::string &problem : result.problems) {
                problems.push_back(*id + ": " + problem);
            }
            continue;
        }
        // A Pack install records no origin (Install.cpp only records native
        // extensions), so record it here for every builtin: the origin is what
        // marks a builtin as trusted and freely loadable.
        if (!record_origin(store_root, *id, Origin::Builtin)) {
            problems.push_back(*id + ": cannot record its origin");
        }
    }
    return problems;
}

} // namespace genesis::extensions
