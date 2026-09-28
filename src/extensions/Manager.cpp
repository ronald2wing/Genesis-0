// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "extensions/Manager.h"

#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "extensions/Consent.h"
#include "extensions/DisabledList.h"
#include "extensions/Install.h"
#include "extensions/ExtensionManifest.h"
#include "extensions/Origins.h"
#include "extensions/RemovedList.h"
#include "extensions/RevokedList.h"
#include "extensions/SourceInstall.h"

namespace genesis::extensions {

namespace {

// The version directory name, as a number. A non-numeric name (a store someone
// edited by hand) counts as 0 so it never wins over a real version. Mirrors the
// file-local helper in ExtensionHost.cpp, which the host's own scan uses.
std::uint32_t parse_version(const std::string &text)
{
    try {
        const unsigned long value = std::stoul(text);
        return value > static_cast<unsigned long>(std::numeric_limits<std::uint32_t>::max())
                ? std::numeric_limits<std::uint32_t>::max()
                : static_cast<std::uint32_t>(value);
    } catch (...) {
        return 0;
    }
}

// The newest installed version directory name for `id`, or nullopt when the id
// has no native extension installed. A non-native (Pack) version directory is
// skipped, matching what the host's scan picks as the loaded version.
std::optional<std::string> newest_version(const std::filesystem::path &store_root,
                                          const std::string &id)
{
    const std::map<std::string, std::map<std::string, std::filesystem::path>> packs =
            installed_packs(store_root);
    const auto it = packs.find(id);
    if (it == packs.end()) {
        return std::nullopt;
    }
    std::optional<std::string> newest;
    std::uint32_t newest_version_number = 0;
    for (const auto &[version, root] : it->second) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(root / "extension.toml", ec)) {
            continue;
        }
        const std::uint32_t parsed = parse_version(version);
        if (!newest || parsed > newest_version_number) {
            newest = version;
            newest_version_number = parsed;
        }
    }
    return newest;
}

} // namespace

Graph read_dependency_graph(const std::filesystem::path &store_root)
{
    Graph graph;
    // Keyed by the installed-packs directory id, which is the manifest id in a
    // normal install. The newest extension manifest per id contributes
    // its edges; a data-only extension (no `entry`) contributes none, and an
    // unusable manifest contributes none (the host's scan already failed it).
    for (const auto &[dir_id, versions] : installed_packs(store_root)) {
        const std::filesystem::path *newest = nullptr;
        std::uint32_t newest_version = 0;
        for (const auto &[version, root] : versions) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(root / "extension.toml", ec)) {
                continue;
            }
            const std::uint32_t parsed = parse_version(version);
            if (newest == nullptr || parsed > newest_version) {
                newest = &root;
                newest_version = parsed;
            }
        }
        if (newest == nullptr) {
            continue;
        }
        const NativeLoadResult loaded = load_native_manifest_file(*newest / "extension.toml");
        if (!loaded.manifest) {
            continue;
        }
        // Every `requires` edge is recorded, including edges naming an id that
        // is not installed: the dependents map then holds the missing id as a
        // key, so a caller can see who waits on it.
        graph.requires_of[dir_id] = loaded.manifest->dependencies;
        for (const std::string &required : loaded.manifest->dependencies) {
            graph.dependents[required].push_back(dir_id);
        }
    }
    return graph;
}

Manager::Manager(std::filesystem::path store_root) : store_root_(std::move(store_root)) { }

std::vector<ExtensionRecord> Manager::list_extensions() const
{
    ExtensionHost host(store_root_, NativeExtension::Host{ });
    host.scan();
    std::vector<ExtensionRecord> records;
    records.reserve(host.records().size() + host.disabled().size() + host.revoked().size()
                    + host.failed().size());
    records.insert(records.end(), host.records().begin(), host.records().end());
    records.insert(records.end(), host.disabled().begin(), host.disabled().end());
    records.insert(records.end(), host.revoked().begin(), host.revoked().end());
    records.insert(records.end(), host.failed().begin(), host.failed().end());
    return records;
}

bool Manager::enable(const std::string &id)
{
    // A revocation is a forced-off: refuse before touching the consent/disable
    // switches. The check uses the loaded (newest) version, the same version the
    // host's scan keys its match on.
    const std::optional<std::string> version = newest_version(store_root_, id);
    if (version && revoked_at(store_root_, id, *version)) {
        return false;
    }
    if (origin_of(id) == Origin::Developer) {
        Consent consent(store_root_);
        return consent.grant(id);
    }
    DisabledList disabled(store_root_);
    return disabled.remove(id);
}

bool Manager::disable(const std::string &id)
{
    if (origin_of(id) == Origin::Developer) {
        Consent consent(store_root_);
        const bool saved = consent.revoke(id);
        disable_dependents(id);
        return saved;
    }
    DisabledList disabled(store_root_);
    const bool saved = disabled.add(id);
    disable_dependents(id);
    return saved;
}

bool Manager::remove_extension(const std::string &id)
{
    if (origin_of(id) == Origin::Builtin) {
        return false; // builtins are first-party; the seed manages them
    }
    // Tombstone first: if the record cannot be written, the copy must stay, or
    // the next seed would silently re-add the extension the user removed.
    if (!remove(store_root_, id)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::remove_all(store_root_ / id, ec);
    disable_dependents(id);
    return !ec;
}

bool Manager::restore_extension(const std::string &id)
{
    if (origin_of(id) == Origin::Builtin) {
        return false; // builtins restore through the CLI's re-seed verb
    }
    return restore(store_root_, id);
}

void Manager::propagate_removed(const std::string &id)
{
    disable_dependents(id);
}

InstallResult Manager::install_from_source(const InstallSource &source, Origin origin)
{
    // Fully qualified: the member name would otherwise hide the free function
    // of the same name in this namespace.
    return genesis::extensions::install_from_source(source, store_root_, origin);
}

Origin Manager::origin_of(const std::string &id) const
{
    const std::map<std::string, Origin> origins = read_origins(store_root_);
    const auto it = origins.find(id);
    return it == origins.end() ? Origin::Developer : it->second;
}

void Manager::disable_dependents(const std::string &id)
{
    const Graph graph = read_dependency_graph(store_root_);
    // Breadth-first over the reverse edges: each dependent is disabled with the
    // reason `requires <parent>` naming the id it directly depends on that just
    // went down. A dependent's own dependents follow, so the cascade is
    // transitive.
    std::vector<std::pair<std::string, std::string>> work; // (id, parent)
    const auto dependents = graph.dependents.find(id);
    if (dependents != graph.dependents.end()) {
        for (const std::string &dependent : dependents->second) {
            work.emplace_back(dependent, id);
        }
    }
    std::size_t index = 0;
    while (index < work.size()) {
        const auto [dependent, parent] = work[index++];
        if (origin_of(dependent) == Origin::Developer) {
            Consent consent(store_root_);
            consent.revoke(dependent);
        } else {
            DisabledList disabled(store_root_);
            disabled.add(dependent, "requires " + parent);
        }
        const auto next = graph.dependents.find(dependent);
        if (next != graph.dependents.end()) {
            for (const std::string &grandchild : next->second) {
                work.emplace_back(grandchild, dependent);
            }
        }
    }
}

} // namespace genesis::extensions
