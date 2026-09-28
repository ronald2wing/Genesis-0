// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Seed.h"

#include <charconv>
#include <optional>
#include <system_error>
#include <utility>

#include "core/ExecutablePath.h"
#include "extensions/Install.h"
#include "extensions/ExtensionManifest.h"
#include "extensions/Origins.h"
#include "extensions/RemovedList.h"
#include "extensions/Trust.h"
#include "workspace/AssetPaths.h"

namespace genesis::extensions {

namespace {

using core::executable_dir;

// The builtin's parsed manifest, or null when the directory holds no valid
// one. Validation is the manifest loader's own, so a non-null manifest means
// the directory is a builtin and its version is trustworthy.
std::optional<NativeLoadResult> builtin_manifest(const std::filesystem::path &dir)
{
    if (std::filesystem::is_regular_file(dir / "extension.toml")) {
        NativeLoadResult loaded = load_native_manifest_file(dir / "extension.toml");
        if (loaded.manifest) {
            return loaded;
        }
    }
    return std::nullopt;
}

// The highest version already installed for `id`, or 0 when the store holds
// none. Installs are laid out as store/<id>/<version>, so the directory names
// are the version record.
int installed_version(const std::filesystem::path &store_root, const std::string &id)
{
    std::error_code ec;
    const std::filesystem::path dir = store_root / id;
    std::filesystem::directory_iterator it(dir, ec);
    const std::filesystem::directory_iterator end;
    int highest = 0;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec) || entry_ec) {
            continue;
        }
        int value = 0;
        const std::string name = it->path().filename().string();
        const auto [end_of_number, parse_ec] =
                std::from_chars(name.data(), name.data() + name.size(), value);
        if (parse_ec == std::errc{ } && end_of_number == name.data() + name.size()
            && value > highest) {
            highest = value;
        }
    }
    return highest;
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

        std::optional<NativeLoadResult> loaded = builtin_manifest(entry.path());
        if (!loaded) {
            problems.push_back("skipping " + name + ": no valid manifest");
            continue;
        }
        const std::string id = loaded->manifest->id;
        const int bundled_version = loaded->manifest->version;

        // A tombstoned id stays gone: the user removed it, so the seed must not
        // silently re-add it on the next startup.
        if (removed(store_root, id)) {
            continue;
        }
        // Re-seeding stays idempotent, but a shipped builtin must be able to
        // carry a fix: install only when the bundled version is newer than
        // everything already installed. An equal or newer install is left
        // alone, so nothing downgrades and nothing is duplicated. Without this
        // an installed builtin could never receive an update.
        if (installed_version(store_root, id) >= bundled_version) {
            continue;
        }

        const InstallResult result =
                install_extension(entry.path(), store_root, Origin::Builtin,
                                  InstallSource{ .type = SourceType::Builtin });
        if (!result.ok()) {
            for (const std::string &problem : result.problems) {
                problems.push_back(id + ": " + problem);
            }
            continue;
        }
        // A Pack install records no origin (Install.cpp only records native
        // extensions), so record it here for every builtin: the origin is what
        // marks a builtin as trusted and freely loadable.
        if (!record_origin(store_root, id, Origin::Builtin)) {
            problems.push_back(id + ": cannot record its origin");
        }
    }
    return problems;
}

} // namespace genesis::extensions
