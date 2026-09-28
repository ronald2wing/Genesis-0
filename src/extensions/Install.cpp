// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Install.h"

#include <cstdint>
#include <system_error>
#include <utility>

#include "effects/PackLoader.h"
#include "extensions/NativeManifest.h"
#include "extensions/Origins.h"
#include "extensions/RemovedList.h"
#include "extensions/Sources.h"

namespace genesis::extensions {

namespace {

// The capabilities a format-2 Pack exercises. Every picture Pack contributes a
// shader the host compiles and runs (`execute`); none declares a filesystem or
// network capability - the shader "cannot do arbitrary IO" (architecture.md
// §9). `pack` is unused because the current schema cannot express a further
// need; it is kept for the same future fields permissions_for reserves.
Permission needs_of(const genesis::effects::Pack &pack)
{
    (void)pack;
    Permission needs;
    needs.execute = true;
    return needs;
}

// True when any capability the Pack needs is withheld by the origin's grant.
bool exceeds(const Permission &needs, const Permission &grant)
{
    return (needs.filesystem && !grant.filesystem) || (needs.network && !grant.network)
            || (needs.execute && !grant.execute);
}

// A directory name used only while an install is in flight, so a reader never
// mistakes a half-installed Extension for a complete one.
bool is_staging(const std::filesystem::path &entry)
{
    return entry.filename().string().starts_with(".tmp-");
}

std::string describe(const genesis::effects::LoadError &problem)
{
    if (problem.where.empty()) {
        return problem.what;
    }
    return problem.where + ": " + problem.what;
}

std::string describe(const NativeLoadError &problem)
{
    if (problem.where.empty()) {
        return problem.what;
    }
    return problem.where + ": " + problem.what;
}

// Copies `source` into `store_root/<id>/<version>` through a staging directory
// and an atomic rename, so a refused or failed install never leaves a partial
// entry. Returns false (and records a problem) on any I/O failure.
bool copy_into_store(const std::string &id, std::uint32_t version,
                     const std::filesystem::path &source, const std::filesystem::path &store_root,
                     std::vector<std::string> &problems)
{
    std::error_code ec;
    std::filesystem::create_directories(store_root, ec);
    if (ec) {
        problems.push_back("cannot create the store: " + ec.message());
        return false;
    }

    const std::filesystem::path staging =
            store_root / (".tmp-" + id + "-" + std::to_string(version));
    std::filesystem::remove_all(staging, ec);
    ec.clear();
    std::filesystem::copy(source, staging, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        problems.push_back("cannot copy the extension: " + ec.message());
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return false;
    }

    const std::filesystem::path id_dir = store_root / id;
    const std::filesystem::path final_path = id_dir / std::to_string(version);
    std::filesystem::create_directories(id_dir, ec);
    if (ec) {
        problems.push_back("cannot create the id directory: " + ec.message());
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return false;
    }

    // Reinstalling the same id+version overwrites deterministically: drop the
    // previous copy before the rename, since rename cannot replace a non-empty
    // directory.
    std::filesystem::remove_all(final_path, ec);
    ec.clear();
    std::filesystem::rename(staging, final_path, ec);
    if (ec) {
        problems.push_back("cannot move the extension into place: " + ec.message());
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        return false;
    }
    return true;
}

// Installs a Pack: validates its effect.toml, applies the Pack trust model
// (is_trusted plus the permission grant), then copies it into the store.
InstallResult install_pack(const std::filesystem::path &source,
                           const std::filesystem::path &store_root, Origin origin,
                           const InstallSource &install_source)
{
    InstallResult result;

    // 1. Load and validate the manifest. load_pack_file runs the schema's own
    //    validation, so a non-empty `problems` covers both "fails to load" and
    //    "fails to validate". Nothing has been written to the store yet.
    const genesis::effects::LoadResult loaded =
            genesis::effects::load_pack_file(source / "effect.toml");
    if (!loaded.problems.empty()) {
        for (const genesis::effects::LoadError &problem : loaded.problems) {
            result.problems.push_back(describe(problem));
        }
        return result;
    }
    const genesis::effects::Pack &pack = *loaded.pack;

    // 2 & 3. Trust and permissions are both policy gates, and a Pack that fails
    //    either is refused. They are collected together rather than
    //    short-circuited, so a human sees every reason at once: with the
    //    current schema (a Pack's only capability is `execute`, granted to the
    //    trusted origins alone) a User Pack fails both, because it is not
    //    trusted and, as a Pack that still contributes a shader, permission-
    //    denied.
    if (!is_trusted(origin)) {
        result.problems.push_back("the Pack's origin is not trusted");
    }
    const Permission needs = needs_of(pack);
    const Permission grant = permissions_for(origin, pack);
    if (exceeds(needs, grant)) {
        result.problems.push_back("the Pack's permissions exceed what its origin grants");
    }
    if (!result.problems.empty()) {
        return result;
    }

    // 4. Every check passed; copy it into place and record its install source.
    //    The source record is best-effort: a failed write still leaves a
    //    complete install, only without a reproducible source.
    if (copy_into_store(pack.id, pack.version, source, store_root, result.problems)) {
        if (!record_source(store_root, pack.id, install_source)) {
            result.problems.push_back("cannot record the Pack's install source in the store");
        }
        result.installed = store_root / pack.id / std::to_string(pack.version);
    }
    return result;
}

// Installs a native extension: validates its extension.toml, applies the native
// trust gate (may_install_native), then copies it into the store. Install is
// "visible" only - the shared library is never loaded here; consent gates the
// later load (see may_load_native and Consent.h).
InstallResult install_native(const std::filesystem::path &source,
                             const std::filesystem::path &store_root, Origin origin,
                             const InstallSource &install_source)
{
    InstallResult result;

    // 1. Load and validate the manifest (the library itself is untouched).
    const NativeLoadResult loaded = load_native_manifest_file(source / "extension.toml");
    if (!loaded.problems.empty()) {
        for (const NativeLoadError &problem : loaded.problems) {
            result.problems.push_back(describe(problem));
        }
        return result;
    }
    const NativeManifest &manifest = *loaded.manifest;

    // 2. Trust gate for a native extension: User is refused, the other three
    //    install. Consent is a load-time concern, not an install-time one.
    if (!may_install_native(origin)) {
        result.problems.push_back("the extension's origin is not trusted");
        return result;
    }

    // 3. Copy it into place and record its origin. The origin record is what
    //    ExtensionHost::scan reads to decide this id's load gate (consent for
    //    Developer, free load for Curated); without it a Curated install would
    //    fall back to Developer and wrongly demand consent. The record is part
    //    of a complete install, so a failed write refuses the install.
    if (copy_into_store(manifest.id, manifest.version, source, store_root, result.problems)) {
        if (!record_origin(store_root, manifest.id, origin)) {
            result.problems.push_back("cannot record the extension's origin in the store");
            return result;
        }
        // A native (re)install is an explicit action, so it clears any
        // removed.json tombstone for the id: a builtin the user reinstalls
        // stops counting as removed and the next seed no longer skips it.
        restore(store_root, manifest.id);
        // The install source record is best-effort (unlike the origin record
        // above, whose failure refuses the install): a failed write still
        // leaves a complete install, only without a reproducible source.
        if (!record_source(store_root, manifest.id, install_source)) {
            result.problems.push_back("cannot record the extension's install source in the store");
        }
        result.installed = store_root / manifest.id / std::to_string(manifest.version);
    }
    return result;
}

} // namespace

InstallResult install_extension(const std::filesystem::path &source,
                                const std::filesystem::path &store_root, Origin origin,
                                InstallSource install_source)
{
    // A Dir install with no recorded path derives the absolute source path, so
    // an install from a local directory records a reproducible source without
    // the caller naming it (the builtin seed and a profile import pass one
    // explicitly instead).
    if (install_source.type == SourceType::Dir && install_source.url.empty()) {
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(source, ec);
        install_source.url = ec ? source.string() : absolute.string();
    }

    // A native extension's manifest is the specific, newer one, so it wins when
    // both are somehow present; a directory holding neither is refused.
    if (std::filesystem::is_regular_file(source / "extension.toml")) {
        return install_native(source, store_root, origin, install_source);
    }
    if (std::filesystem::is_regular_file(source / "effect.toml")) {
        return install_pack(source, store_root, origin, install_source);
    }
    InstallResult result;
    result.problems.push_back("no extension.toml or effect.toml in " + source.string());
    return result;
}

std::map<std::string, std::map<std::string, std::filesystem::path>>
installed_packs(const std::filesystem::path &store_root)
{
    std::map<std::string, std::map<std::string, std::filesystem::path>> out;

    std::error_code ec;
    if (!std::filesystem::is_directory(store_root, ec) || ec) {
        return out;
    }

    const std::filesystem::directory_iterator end;
    std::filesystem::directory_iterator id_it(store_root, ec);
    for (; !ec && id_it != end; id_it.increment(ec)) {
        const std::filesystem::directory_entry id_entry = *id_it;
        if (is_staging(id_entry.path()) || !id_entry.is_directory(ec)) {
            continue;
        }
        const std::string id = id_entry.path().filename().string();

        std::error_code version_ec;
        std::filesystem::directory_iterator version_it(id_entry.path(), version_ec);
        for (; !version_ec && version_it != end; version_it.increment(version_ec)) {
            const std::filesystem::directory_entry version_entry = *version_it;
            if (is_staging(version_entry.path()) || !version_entry.is_directory(version_ec)) {
                continue;
            }
            out[id][version_entry.path().filename().string()] = version_entry.path();
        }
    }
    return out;
}

} // namespace genesis::extensions
