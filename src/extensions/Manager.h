// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "extensions/ExtensionHost.h"
#include "extensions/Install.h"
#include "extensions/Trust.h"

namespace genesis::extensions {

// The installed store's dependency graph: every manifest's `requires` edges,
// including edges that name an id that is not installed (a missing
// dependency). `requires_of[id]` lists the ids `id` depends on; `dependents[dep]`
// lists the installed ids that declare they depend on `dep`. Both are read
// from the newest manifest per id, the same resolution ExtensionHost::scan
// uses, so the graph and the host's records agree.
struct Graph
{
    std::map<std::string, std::vector<std::string>> requires_of;
    std::map<std::string, std::vector<std::string>> dependents;
};

// Reads the dependency graph from the installed store. Packs (an effect.toml)
// are skipped; only native extensions (an extension.toml) contribute edges.
Graph read_dependency_graph(const std::filesystem::path &store_root);

// The store's manager: the enable/disable/remove/restore switch over installed
// native extensions, keyed by origin, and the resolved list the app/API
// surface. It is stateless over `store_root` (each call re-reads the store),
// so it can be shared and constructed per operation.
//
// Enable/disable route by origin: a Developer extension is consent-gated
// (enable == grant, disable == revoke); a Builtin/Curated extension is toggled
// on the store's disabled list. Disabling an extension also disables every
// transitive dependent, recording `requires <parent>` as the reason, so a
// dependent's disappearance is explained rather than a silent failure.
//
// Remove/restore refuse a Builtin extension (builtins are first-party and are
// managed by the seed, not removed): remove tombstones a Curated/Developer id
// (so the seed never re-adds it), deletes its installed copy, and disables its
// dependents; restore clears the tombstone.
class Manager
{
public:
    explicit Manager(std::filesystem::path store_root);

    // The installed native extensions, enabled first then disabled then failed,
    // each carrying its requires/dependents and load state. Reuses
    // ExtensionHost::scan (with no bridge or log sink), so the list is exactly
    // the state the host would load.
    std::vector<ExtensionRecord> list_extensions() const;

    // Enable/disable `id` per its origin. Returns false only when the store
    // write fails; the in-memory switch still applies for this process.
    bool enable(const std::string &id);
    bool disable(const std::string &id);

    // Remove/restore `id`. A Builtin id is refused (returns false); a
    // Curated/Developer id is tombstoned (remove) or un-tombstoned (restore).
    // Remove also deletes the installed copy and disables the dependents.
    bool remove_extension(const std::string &id);
    bool restore_extension(const std::string &id);

    // Disables every transitive dependent of `id`, recording `requires <parent>`
    // as the reason. Used by remove (and the CLI's builtin remove) so that
    // removing an extension does not leave its dependents loading against a
    // missing dependency.
    void propagate_removed(const std::string &id);

    // Installs an extension from a source (a directory or a git repository)
    // into the store under `origin`, through install_from_source. The trust
    // gate is install_from_source's unchanged; a refusal is reported in the
    // result's problems, never a process error.
    InstallResult install_from_source(const InstallSource &source, Origin origin);

private:
    // The origin recorded for `id`, or Developer when none is recorded.
    Origin origin_of(const std::string &id) const;

    // The shared dependent cascade behind disable and propagate_removed.
    void disable_dependents(const std::string &id);

    std::filesystem::path store_root_;
};

} // namespace genesis::extensions
